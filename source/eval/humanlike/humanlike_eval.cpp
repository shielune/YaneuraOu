#include "humanlike_eval.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "../../bitboard.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../../misc.h"

namespace YaneuraOu {
namespace Eval {
namespace HumanLike {

const std::vector<std::string> mode_names = {
	"nnue",
	"MT",
	"MB",
	"KPL",
	"HalfKPL",
};

Mode parse_mode(const std::string& s) {
	if (s == "MT")       return Mode::Material;
	if (s == "MB")       return Mode::Mobility;
	if (s == "KPL")      return Mode::KPL;
	if (s == "HalfKPL")  return Mode::HalfKPL;
	return Mode::Nnue;
}

namespace {

// MT: 標準 PieceValue による駒得合計 (先手視点)。
int compute_material(const Position& pos) {
	int score = 0;
	for (Square sq = SQ_ZERO; sq < SQ_NB; ++sq) {
		Piece pc = pos.piece_on(sq);
		if (pc != NO_PIECE)
			score += Eval::PieceValue[pc];
	}
	for (Color c = BLACK; c < COLOR_NB; ++c) {
		Hand h = pos.hand_of(c);
		int sign = (c == BLACK) ? 1 : -1;
		for (PieceType pt = PAWN; pt < PIECE_HAND_NB; ++pt)
			score += sign * hand_count(h, pt) * Eval::PieceValue[pt];
	}
	return score;
}

// ============================================================================
// MB: 駒種×距離×方向 learned weight 評価 (164 params)
// ============================================================================
//
// Feature index layout:
//   [0, 107): 盤上駒の (pt, |df|, dr_canonical) → index
//             dr_canonical は先手フレームに正規化 (後手は dr 反転)
//   [107, 164): 持ち駒の (hand_pt, |df|, dr_canonical) → index
//             持ち駒側は「全 legal drop 升 × その升からの利き」を全展開した合計
//
// 駒種共有規則:
//   - 金型 (PRO_PAWN/PRO_LANCE/PRO_KNIGHT/PRO_SILVER) は GOLD と同じ index に
//   - 馬 (HORSE) は独立、龍 (DRAGON) は独立
//   - 玉 (KING) は単独
//
// 左右対称: df の符号は absolute 化して mirror 統一。

constexpr int OFFSET_RANGE = 17;
constexpr int OFFSET_BIAS  = 8;

int g_board_idx_table[PIECE_TYPE_NB][OFFSET_RANGE][OFFSET_RANGE];
int g_hand_idx_table [PIECE_HAND_NB][OFFSET_RANGE][OFFSET_RANGE];

// 土台の 164 列の名前。番号のままだと `mobility.37` のようになって、人にも
// LLM にも何の利きか伝わらない。番号を振るのと同じ場所で名前も控えておく。
// 盤上は `銀(1,-1)`、持ち駒は打った先の利きなので `打銀(1,-1)` と書き分ける。
std::string g_board_slot_name[MAX_BOARD_FEATURES];
std::string g_hand_slot_name[NUM_HAND_FEATURES];

// 名前に出す駒の字。金の動きをするものは登録の時点で GOLD に寄せてあるので、
// PRO_PAWN などがここへ来ることはない。
const char* piece_kanji(PieceType pt) {
	switch (pt) {
	case PAWN:   return "歩";
	case LANCE:  return "香";
	case KNIGHT: return "桂";
	case SILVER: return "銀";
	case GOLD:   return "金";
	case BISHOP: return "角";
	case ROOK:   return "飛";
	case KING:       return "玉";
	case PRO_PAWN:   return "と";
	case PRO_LANCE:  return "成香";
	case PRO_KNIGHT: return "成桂";
	case PRO_SILVER: return "成銀";
	case HORSE:      return "馬";
	case DRAGON:     return "龍";
	default:         return "?";
	}
}

// (df, dr) は先手フレームに正規化した後の値。df は絶対値にしてあるので、
// 左右の区別は付かない (左右対称で一つの列にまとめているため)。
std::string slot_label(const char* kanji, int df, int dr) {
	return std::string(kanji) + "(" + std::to_string(df) + "," + std::to_string(dr) + ")";
}

std::atomic<bool> g_index_inited{false};
std::mutex g_init_mutex;

void init_index_tables() {
	std::lock_guard<std::mutex> lk(g_init_mutex);
	if (g_index_inited.load(std::memory_order_acquire))
		return;

	for (int pt = 0; pt < PIECE_TYPE_NB; ++pt)
		for (int i = 0; i < OFFSET_RANGE; ++i)
			for (int j = 0; j < OFFSET_RANGE; ++j)
				g_board_idx_table[pt][i][j] = -1;
	for (int pt = 0; pt < PIECE_HAND_NB; ++pt)
		for (int i = 0; i < OFFSET_RANGE; ++i)
			for (int j = 0; j < OFFSET_RANGE; ++j)
				g_hand_idx_table[pt][i][j] = -1;

	int idx = 0;
	auto add_board = [&](PieceType pt, int df, int dr) {
		df = std::abs(df);
		int& slot = g_board_idx_table[pt][df + OFFSET_BIAS][dr + OFFSET_BIAS];
		if (slot < 0) {
			g_board_slot_name[idx] = slot_label(piece_kanji(pt), df, dr);
			slot = idx++;
		}
	};
	auto alias_to = [&](PieceType pt, int df, int dr, int target_idx) {
		df = std::abs(df);
		g_board_idx_table[pt][df + OFFSET_BIAS][dr + OFFSET_BIAS] = target_idx;
	};

	// 歩
	add_board(PAWN, 0, -1);
	// 香: forward 1-8
	for (int d = 1; d <= 8; ++d) add_board(LANCE, 0, -d);
	// 桂
	add_board(KNIGHT, 1, -2);
	// 銀: 前 / 前斜 / 後斜
	add_board(SILVER, 0, -1);
	add_board(SILVER, 1, -1);
	add_board(SILVER, 1,  1);
	// 金型: 前 / 前斜 / 横 / 後 (PRO_* は GOLD と alias)
	{
		const std::array<std::pair<int,int>, 4> gold_offs = {{ {0,-1}, {1,-1}, {1,0}, {0,1} }};
		for (auto [df, dr] : gold_offs)
			add_board(GOLD, df, dr);
	}
	// 玉: 8 offsets → mirror 5
	add_board(KING, 0, -1);
	add_board(KING, 1, -1);
	add_board(KING, 1,  0);
	add_board(KING, 1,  1);
	add_board(KING, 0,  1);
	// 角: 前斜 / 後斜 × 8 距離
	for (int d = 1; d <= 8; ++d) {
		add_board(BISHOP, d, -d);
		add_board(BISHOP, d,  d);
	}
	// 飛: 前 / 後 / 横 × 8 距離
	for (int d = 1; d <= 8; ++d) {
		add_board(ROOK, 0, -d);
		add_board(ROOK, 0,  d);
		add_board(ROOK, d,  0);
	}
	// 馬: 角部分 16 + 直交隣接 3
	for (int d = 1; d <= 8; ++d) {
		add_board(HORSE, d, -d);
		add_board(HORSE, d,  d);
	}
	add_board(HORSE, 0, -1);
	add_board(HORSE, 0,  1);
	add_board(HORSE, 1,  0);
	// 龍: 飛部分 24 + 斜め隣接 2
	for (int d = 1; d <= 8; ++d) {
		add_board(DRAGON, 0, -d);
		add_board(DRAGON, 0,  d);
		add_board(DRAGON, d,  0);
	}
	add_board(DRAGON, 1, -1);
	add_board(DRAGON, 1,  1);

	// Split-only columns are appended so folded indices 0..106 stay unchanged.
	const std::array<std::pair<int,int>, 4> gold_offs = {{ {0,-1}, {1,-1}, {1,0}, {0,1} }};
	for (PieceType pt : {PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER})
		for (auto [df, dr] : gold_offs)
			add_board(pt, df, dr);

	if (idx != MAX_BOARD_FEATURES) {
		std::cerr << "humanlike_eval: board index count mismatch: " << idx
		          << " vs " << MAX_BOARD_FEATURES << std::endl;
		std::abort();
	}

	int hand_idx = 0;
	auto add_hand = [&](PieceType pt, int df, int dr) {
		df = std::abs(df);
		int& slot = g_hand_idx_table[pt][df + OFFSET_BIAS][dr + OFFSET_BIAS];
		if (slot < 0) {
			g_hand_slot_name[hand_idx] = std::string("打") + slot_label(piece_kanji(pt), df, dr);
			slot = hand_idx++;
		}
	};
	add_hand(PAWN, 0, -1);
	for (int d = 1; d <= 8; ++d) add_hand(LANCE, 0, -d);
	add_hand(KNIGHT, 1, -2);
	add_hand(SILVER, 0, -1); add_hand(SILVER, 1, -1); add_hand(SILVER, 1, 1);
	add_hand(GOLD, 0, -1); add_hand(GOLD, 1, -1); add_hand(GOLD, 1, 0); add_hand(GOLD, 0, 1);
	for (int d = 1; d <= 8; ++d) {
		add_hand(BISHOP, d, -d);
		add_hand(BISHOP, d,  d);
	}
	for (int d = 1; d <= 8; ++d) {
		add_hand(ROOK, 0, -d);
		add_hand(ROOK, 0,  d);
		add_hand(ROOK, d,  0);
	}

	// ここで振る番号は盤上 107 と持ち駒 57 の分だけ。段・升・先で掛けた列は
	// 番号表を持たず、この番号から Layout の offset で引き伸ばして足す。
	if (hand_idx != NUM_HAND_FEATURES) {
		std::cerr << "humanlike_eval: hand index count mismatch: " << hand_idx
		          << " vs " << NUM_HAND_FEATURES << std::endl;
		std::abort();
	}

	g_index_inited.store(true, std::memory_order_release);
}

// MB の学習済 weight。default 0 → 評価値 0 (loadなしだと中立)。plain の 164 個。
float g_mobility_weights[NUM_BOARD_FEATURES_FOLDED + NUM_HAND_FEATURES] = {0};
std::atomic<bool> g_mobility_weights_loaded{false};

// 持ち駒の rank 制約: 歩・香は最敵陣不可、桂は最敵陣 + 1 段手前まで不可。
bool drop_legal_rank(Color c, PieceType pt, Rank r) {
	Rank rel = (c == BLACK) ? r : (Rank)(RANK_9 - r);
	if (pt == PAWN || pt == LANCE)
		return rel != RANK_1;
	if (pt == KNIGHT)
		return rel != RANK_1 && rel != RANK_2;
	return true;
}

// 利きの指す升に何が居るかを 0..20 に潰す。前半 10 が自分の駒、次の 10 が
// 相手の駒、最後の 1 が空白。c は利かせている側。
inline int contact_slot(const Position& pos, Square ts, Color c, const Layout& layout) {
	const Piece tp = pos.piece_on(ts);
	if (tp == NO_PIECE) return layout.contact_pieces * 2;
	const int enemy = (color_of(tp) == c) ? 0 : 1;
	return enemy * layout.contact_pieces + contact_piece_index(type_of(tp), layout.pieces);
}

// 利きを一度だけ走査して、版の並びでの (列, 値) を sink に渡す。
// feature ベクトルを作る側も内積を取る側もここを通るので、二つが食い違わない。
template <typename Sink>
void walk_mobility(const Position& pos, const Layout& layout, Sink&& sink) {
	if (!g_index_inited.load(std::memory_order_acquire))
		init_index_tables();

	const Bitboard occupied = pos.pieces();

	// 盤上駒
	Bitboard all = occupied;
	while (all) {
		Square sq = all.pop();
		Piece pc = pos.piece_on(sq);
		PieceType pt = type_of(pc);
		Color c = color_of(pc);
		const float sign = (c == BLACK) ? 1.0f : -1.0f;
		const int sf = (int)file_of(sq), sr = (int)rank_of(sq);
		// 段と升は「利かせている駒の側から見て」なので、後手は 180 度回す。
		const int ysq        = sf * 9 + sr;
		const int own_rank   = (c == BLACK) ? sr : 8 - sr;
		const int own_square = (c == BLACK) ? ysq : 80 - ysq;

		Bitboard atk = effects_from(pc, sq, occupied);
		while (atk) {
			Square ts = atk.pop();
			int df = (int)file_of(ts) - sf;
			int dr = (int)rank_of(ts) - sr;
			if (c == WHITE) dr = -dr;
			int at = g_board_idx_table[pt][std::abs(df) + OFFSET_BIAS][dr + OFFSET_BIAS];
			if (layout.pieces == Pieces::Folded
			    && (pt == PRO_PAWN || pt == PRO_LANCE || pt == PRO_KNIGHT || pt == PRO_SILVER))
				at = g_board_idx_table[GOLD][std::abs(df) + OFFSET_BIAS][dr + OFFSET_BIAS];
			if (at < 0) continue;

			if (layout.cross == 9)
				sink(layout.off_cross + at * 9 + own_rank, sign);
			else if (layout.cross == 81)
				sink(layout.off_cross + at * 81 + own_square, sign);
			else if (!layout.contact)
				sink(at, sign);

			if (layout.contact)
				sink(layout.off_contact_board + at * layout.contact_dim
				         + contact_slot(pos, ts, c, layout),
				     sign);
		}
	}

	// 持ち駒: 全 legal drop 升 × そこからの利き
	for (Color c = BLACK; c < COLOR_NB; ++c) {
		Hand h = pos.hand_of(c);
		float sign = (c == BLACK) ? 1.0f : -1.0f;
		for (PieceType pt = PAWN; pt < PIECE_HAND_NB; ++pt) {
			int cnt = hand_count(h, pt);
			if (cnt == 0) continue;
			Piece pc = make_piece(c, pt);

			bool pawn_files[9] = {false};
			if (pt == PAWN) {
				Bitboard pb = pos.pieces(c, PAWN);
				while (pb) {
					Square s = pb.pop();
					pawn_files[(int)file_of(s)] = true;
				}
			}

			for (Square sq = SQ_ZERO; sq < SQ_NB; ++sq) {
				if (pos.piece_on(sq) != NO_PIECE) continue;
				Rank r = rank_of(sq);
				if (!drop_legal_rank(c, pt, r)) continue;
				if (pt == PAWN && pawn_files[(int)file_of(sq)]) continue;

				Bitboard atk = effects_from(pc, sq, occupied);
				const int sf = (int)file_of(sq), sr = (int)rank_of(sq);
				while (atk) {
					Square ts = atk.pop();
					int df = (int)file_of(ts) - sf;
					int dr = (int)rank_of(ts) - sr;
					if (c == WHITE) dr = -dr;
					const int at = g_hand_idx_table[pt][std::abs(df) + OFFSET_BIAS][dr + OFFSET_BIAS];
					if (at < 0) continue;
					const int rel = at;
					// 打った先の升にも中身がある。持ち駒の 57 列も先で分ける。
					if (layout.contact)
						sink(layout.off_contact_hand + rel * layout.contact_dim
						         + contact_slot(pos, ts, c, layout),
						     sign * (float)cnt);
					else
						sink(layout.off_hand + rel, sign * (float)cnt);
				}
			}
		}
	}
}

} // anonymous namespace

const char* variant_name(Variant v) {
	switch (v) {
	case Variant::Plain:         return "plain";
	case Variant::Rank:          return "rank";
	case Variant::Square:        return "square";
	case Variant::Contact:       return "contact";
	case Variant::RankContact:   return "rank+contact";
	case Variant::SquareContact: return "square+contact";
	}
	return "plain";
}

bool parse_variant(const std::string& s, Variant& out) {
	for (int i = 0; i < NUM_VARIANTS; ++i) {
		if (s == variant_name((Variant)i)) {
			out = (Variant)i;
			return true;
		}
	}
	return false;
}

const char* pieces_name(Pieces p) { return p == Pieces::Folded ? "folded" : "split"; }
bool parse_pieces(const std::string& s, Pieces& out) {
	if (s == "folded") { out = Pieces::Folded; return true; }
	if (s == "split") { out = Pieces::Split; return true; }
	return false;
}

Layout make_layout(Variant v, Pieces p) {
	Layout L;
	L.variant = v; L.pieces = p;
	L.board = p == Pieces::Folded ? NUM_BOARD_FEATURES_FOLDED : NUM_BOARD_FEATURES_SPLIT;
	L.contact_pieces = p == Pieces::Folded ? NUM_CONTACT_PIECES_FOLDED : NUM_CONTACT_PIECES_SPLIT;
	L.contact_dim = L.contact_pieces * 2 + 1;
	L.material = p == Pieces::Folded ? NUM_MATERIAL_FOLDED : NUM_MATERIAL_SPLIT;
	L.cross = (v == Variant::Rank || v == Variant::RankContact) ? 9
	        : (v == Variant::Square || v == Variant::SquareContact) ? 81 : 0;
	L.contact = v == Variant::Contact || v == Variant::RankContact || v == Variant::SquareContact;
	const int front = L.board * L.cross;
	L.off_cross = 0; L.off_contact_board = front;
	L.off_contact_hand = front + L.board * L.contact_dim;
	if (L.contact) { L.off_hand = 0; L.mobility = L.off_contact_hand + NUM_HAND_FEATURES * L.contact_dim; }
	else if (L.cross == 0) { L.off_hand = L.board; L.mobility = L.board + NUM_HAND_FEATURES; }
	else { L.off_hand = front; L.mobility = front + NUM_HAND_FEATURES; }
	return L;
}

bool layout_for_dim(int dim, Layout& out, int& kind) {
	for (int p = 0; p < 2; ++p) for (int i = 0; i < NUM_VARIANTS; ++i) {
		const Layout L = make_layout((Variant)i, (Pieces)p);
		if (dim == L.mobility) { out=L; kind=0; return true; }
		if (dim == L.features()) { out=L; kind=1; return true; }
		if (dim == L.tapered()) { out=L; kind=2; return true; }
	}
	return false;
}

void extract_features(const Position& pos, const Layout& layout, float* feat) {
	for (int i = 0; i < layout.mobility; ++i) feat[i] = 0.0f;
	walk_mobility(pos, layout, [&](int at, float v) { feat[at] += v; });
}

double mobility_dot(const Position& pos, const Layout& layout,
                    const float* w0, const float* w1, float t) {
	double sum = 0.0;
	if (w1 == nullptr) {
		walk_mobility(pos, layout,
		              [&](int at, float v) { sum += (double)w0[at] * (double)v; });
	} else {
		const double a = 1.0 - (double)t, b = (double)t;
		walk_mobility(pos, layout, [&](int at, float v) {
			sum += ((double)w0[at] * a + (double)w1[at] * b) * (double)v;
		});
	}
	return sum;
}

bool load_mobility_weights(const std::string& path) {
	if (!g_index_inited.load(std::memory_order_acquire))
		init_index_tables();

	std::ifstream ifs(path);
	if (!ifs) {
		std::cerr << "humanlike_eval[MB]: failed to open " << path << std::endl;
		return false;
	}
	std::vector<float> tmp;
	tmp.reserve(NUM_BOARD_FEATURES_FOLDED + NUM_HAND_FEATURES);
	std::string line;
	while (std::getline(ifs, line)) {
		size_t p = line.find_first_not_of(" \t\r\n");
		if (p == std::string::npos) continue;
		if (line[p] == '#') continue;
		const char* begin = line.c_str() + p;
		char* end = nullptr;
		float v = std::strtof(begin, &end);
		if (end == begin) {
			std::cerr << "humanlike_eval[MB]: parse error: " << line << std::endl;
			return false;
		}
		tmp.push_back(v);
	}
	// MB は plain の 164 列だけを使う。段・升・先で掛けた版は HceWeightsFile
	// (evaluate_mobility.cpp) の経路で読む。
	if (tmp.size() != (size_t)NUM_BOARD_FEATURES_FOLDED + NUM_HAND_FEATURES) {
		std::cerr << "humanlike_eval[MB]: " << tmp.size()
		          << " entries, expected " << NUM_BOARD_FEATURES_FOLDED + NUM_HAND_FEATURES << std::endl;
		return false;
	}
	for (size_t i = 0; i < (size_t)NUM_BOARD_FEATURES_FOLDED + NUM_HAND_FEATURES; ++i)
		g_mobility_weights[i] = tmp[i];
	g_mobility_weights_loaded.store(true, std::memory_order_release);
	std::cerr << "humanlike_eval[MB]: loaded " << tmp.size() << " weights from " << path << std::endl;
	return true;
}

// ============================================================================
// KPL: K+P 線形 (1,710 params)
// ============================================================================
//
//   index ∈ [0, 1548)      … P (玉以外の BonaPiece、fb 視点)
//   index ∈ [1548, 1710)   … K (BLACK 玉位置 + 81 + WHITE 玉位置)
//
//   pieces_fb[i]               (i ∈ [0,38))   → P index = pieces_fb[i]
//   pieces_fb[PIECE_NUMBER_BKING] - fe_end    → BLACK K offset ∈ [0, 81)
//   pieces_fb[PIECE_NUMBER_WKING] - fe_end    → WHITE K offset ∈ [81, 162)
// pieces_fb は f_king + sq 形式で、BLACK と WHITE の king は別オフセットで
// 入っているのでそのまま K 領域に並ぶ。

namespace {

std::vector<float> g_kpl_weights;
std::atomic<bool>  g_kpl_weights_loaded{false};

} // anonymous namespace

int extract_kpl_indices(const Position& pos, int* idx_out) {
	const BonaPiece* pieces = pos.eval_list()->piece_list_fb();
	int n = 0;
	// P 部分 (38 駒)
	for (PieceNumber i = PIECE_NUMBER_ZERO; i < PIECE_NUMBER_KING; ++i) {
		idx_out[n++] = (int)pieces[i];
	}
	// K 部分 (2 玉) — fe_end (= 1548) 引いて K block 先頭にシフト
	for (PieceNumber i = PIECE_NUMBER_KING; i < PIECE_NUMBER_NB; ++i) {
		idx_out[n++] = (int)pieces[i] - (int)Eval::fe_end + NUM_KPL_P_FEATURES;
	}
	return n; // 40
}

bool load_kpl_weights(const std::string& path) {
	std::ifstream ifs(path);
	if (!ifs) {
		std::cerr << "humanlike_eval[KPL]: failed to open " << path << std::endl;
		return false;
	}
	std::vector<float> tmp;
	tmp.reserve(NUM_KPL_FEATURES);
	std::string line;
	while (std::getline(ifs, line)) {
		size_t p = line.find_first_not_of(" \t\r\n");
		if (p == std::string::npos) continue;
		if (line[p] == '#') continue;
		const char* begin = line.c_str() + p;
		char* end = nullptr;
		float v = std::strtof(begin, &end);
		if (end == begin) {
			std::cerr << "humanlike_eval[KPL]: parse error: " << line << std::endl;
			return false;
		}
		tmp.push_back(v);
	}
	if ((int)tmp.size() != NUM_KPL_FEATURES) {
		std::cerr << "humanlike_eval[KPL]: " << tmp.size()
		          << " entries, expected " << NUM_KPL_FEATURES << std::endl;
		return false;
	}
	g_kpl_weights = std::move(tmp);
	g_kpl_weights_loaded.store(true, std::memory_order_release);
	std::cerr << "humanlike_eval[KPL]: loaded " << NUM_KPL_FEATURES
	          << " weights from " << path << std::endl;
	return true;
}

namespace {

int kpl_score(const Position& pos) {
	if (!g_kpl_weights_loaded.load(std::memory_order_acquire))
		return 0;
	int idx[64];
	const int n = extract_kpl_indices(pos, idx);
	double sum = 0.0;
	for (int i = 0; i < n; ++i)
		sum += (double)g_kpl_weights[idx[i]];
	if (sum >  3000.0) sum =  3000.0;
	if (sum < -3000.0) sum = -3000.0;
	return (int)sum;
}

} // anonymous namespace

// ============================================================================
// HalfKPL: HalfKP cross-product linear (125,388 params, Friend - Enemy)
// ============================================================================
//
//   friend 側: anchor = BLACK king sq, pieces = pieces_fb
//     idx = sq_k_friend * fe_end + pieces_fb[i]
//   enemy 側: anchor = WHITE king sq の白フレーム mirror, pieces = pieces_fw
//     idx = sq_k_enemy  * fe_end + pieces_fw[i]
//
//   sq_k_friend は pieces_fb[PIECE_NUMBER_BKING] - f_king ∈ [0, 81)
//   sq_k_enemy  は pieces_fw[PIECE_NUMBER_WKING] - f_king ∈ [0, 81)
//   (half_kp.cpp の GetPieces と同じ計算)

namespace {

std::vector<float> g_halfkpl_weights;
std::atomic<bool>  g_halfkpl_weights_loaded{false};

} // anonymous namespace

void extract_halfkpl_indices(const Position& pos,
                              int* friend_out, int* enemy_out) {
	const BonaPiece* fb = pos.eval_list()->piece_list_fb();
	const BonaPiece* fw = pos.eval_list()->piece_list_fw();
	const int sq_k_friend = (int)fb[PIECE_NUMBER_BKING] - (int)Eval::f_king;
	const int sq_k_enemy  = (int)fw[PIECE_NUMBER_WKING] - (int)Eval::f_king;
	for (PieceNumber i = PIECE_NUMBER_ZERO; i < PIECE_NUMBER_KING; ++i) {
		friend_out[i] = sq_k_friend * NUM_HKPL_FE_END + (int)fb[i];
		enemy_out [i] = sq_k_enemy  * NUM_HKPL_FE_END + (int)fw[i];
	}
}

bool load_halfkpl_weights(const std::string& path) {
	std::ifstream ifs(path);
	if (!ifs) {
		std::cerr << "humanlike_eval[HalfKPL]: failed to open " << path << std::endl;
		return false;
	}
	std::vector<float> tmp;
	tmp.reserve(NUM_HKPL_FEATURES);
	std::string line;
	while (std::getline(ifs, line)) {
		size_t p = line.find_first_not_of(" \t\r\n");
		if (p == std::string::npos) continue;
		if (line[p] == '#') continue;
		const char* begin = line.c_str() + p;
		char* end = nullptr;
		float v = std::strtof(begin, &end);
		if (end == begin) {
			std::cerr << "humanlike_eval[HalfKPL]: parse error: " << line << std::endl;
			return false;
		}
		tmp.push_back(v);
	}
	if ((int)tmp.size() != NUM_HKPL_FEATURES) {
		std::cerr << "humanlike_eval[HalfKPL]: " << tmp.size()
		          << " entries, expected " << NUM_HKPL_FEATURES << std::endl;
		return false;
	}
	g_halfkpl_weights = std::move(tmp);
	g_halfkpl_weights_loaded.store(true, std::memory_order_release);
	std::cerr << "humanlike_eval[HalfKPL]: loaded " << NUM_HKPL_FEATURES
	          << " weights from " << path << std::endl;
	return true;
}

namespace {

int halfkpl_score(const Position& pos) {
	if (!g_halfkpl_weights_loaded.load(std::memory_order_acquire))
		return 0;
	int friend_idx[PIECE_NUMBER_KING];
	int enemy_idx [PIECE_NUMBER_KING];
	extract_halfkpl_indices(pos, friend_idx, enemy_idx);
	double sum = 0.0;
	for (int i = 0; i < PIECE_NUMBER_KING; ++i)
		sum += (double)g_halfkpl_weights[friend_idx[i]];
	for (int i = 0; i < PIECE_NUMBER_KING; ++i)
		sum -= (double)g_halfkpl_weights[enemy_idx[i]];
	if (sum >  3000.0) sum =  3000.0;
	if (sum < -3000.0) sum = -3000.0;
	return (int)sum;
}

int mobility_score(const Position& pos) {
	static const Layout plain = make_layout(Variant::Plain);
	double sum = mobility_dot(pos, plain, g_mobility_weights, nullptr, 0.0f);
	if (sum >  3000.0) sum =  3000.0;
	if (sum < -3000.0) sum = -3000.0;
	return (int)sum;
}

} // anonymous namespace

Value evaluate(const Position& pos, Mode mode, bool capture_force) {
	int score = 0;
	switch (mode) {
	case Mode::Material:
		score = compute_material(pos);
		break;
	case Mode::Mobility:
		score = mobility_score(pos);
		break;
	case Mode::KPL:
		score = kpl_score(pos);
		break;
	case Mode::HalfKPL:
		score = halfkpl_score(pos);
		break;
	case Mode::Nnue:
	default:
		return VALUE_ZERO;
	}
	(void)capture_force;
	return Value(pos.side_to_move() == BLACK ? score : -score);
}

bool is_free_capture(const Position& pos, Move m) {
	if (!pos.capture(m))
		return false;
	Square to = m.to_sq();
	Color us = pos.side_to_move();
	Color them = ~us;
	int my_atk  = pos.attackers_to(us,   to).pop_count();
	int opp_atk = pos.attackers_to(them, to).pop_count();
	return my_atk > opp_atk;
}

// ===========================================================================
// HCE v2: 駒得 17 + 利き 185 + 玉の安全度 50 = 252 次元
// ===========================================================================
//
// 玉の安全度の原典は tideborn/king_safety/features.py の _king_features。
// 向こうは cshogi (dlshogi native encoder) の 31 plane / 片側 を使う:
//
//   plane  0..13  … 駒種ごとの占有 (PieceType - 1 が plane 番号。cshogi と
//                   やねうら王の PieceType の番号は一致している)
//   plane 14..27  … 駒種ごとの利きの和 (2 値)。自駒のいる升も 1 になる
//                   (= effects_from(pc, sq, occupied) そのもの)
//   plane 28..30  … 利き「枚数」が 1 枚以上 / 2 枚以上 / 3 枚以上 (2 値)
//
// guard_types は plane 14..27 の和から玉の分 (plane 21) を引いたもの、つまり
// 「その升に利かせている自分の駒の *種類数*」。enemy_attack_ge{1,2,3} のほうは
// *枚数* なので、この二つを混ぜてはいけない。
//
// dlshogi は後手番のとき盤を 180 度回すが、ここで数えるのはチェビシェフ距離の
// 輪とその中の個数だけで、どれも回転で変わらない。よって盤を回さずに直接数える。

namespace {

// dlshogi の plane 0..13 に対応する PieceType (PIECE_NAMES の順)。
constexpr PieceType kPlanePieceType[14] = {
	PAWN, LANCE, KNIGHT, SILVER, BISHOP, ROOK, GOLD, KING,
	PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER, HORSE, DRAGON
};

// 持ち駒の並び。cshogi の pieces_in_hand と同じ順で、やねうら王の PieceType 順
// (PAWN..LANCE..KNIGHT..SILVER..BISHOP..ROOK..GOLD) とは金・角・飛の位置が違う。
constexpr PieceType kHandOrder[7] = { PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK };

// 進行度で使う駒の重み。成り駒は成る前の値。玉は 0。
constexpr int kPhaseWeight[PIECE_TYPE_NB] = {
	0, // NO_PIECE_TYPE
	1, // PAWN
	2, // LANCE
	2, // KNIGHT
	3, // SILVER
	5, // BISHOP
	5, // ROOK
	4, // GOLD
	0, // KING
	1, // PRO_PAWN
	2, // PRO_LANCE
	2, // PRO_KNIGHT
	3, // PRO_SILVER
	5, // HORSE
	5, // DRAGON
	0, // QUEEN
};

// 片側ぶんの利き情報。
struct SideAttacks {
	Bitboard type_atk[PIECE_TYPE_NB]; // 駒種ごとの利きの和 (2 値)
	uint8_t  atk_count[SQ_NB];        // 利かせている駒の枚数
	uint8_t  guard_types[SQ_NB];      // 利かせている駒の種類数 (玉を除く)
};

void build_side_attacks(const Position& pos, SideAttacks* out) {
	const Bitboard occupied = pos.pieces();

	for (Color c = BLACK; c < COLOR_NB; ++c) {
		for (int pt = 0; pt < PIECE_TYPE_NB; ++pt)
			out[c].type_atk[pt] = Bitboard(0);
		std::memset(out[c].atk_count,   0, sizeof(out[c].atk_count));
		std::memset(out[c].guard_types, 0, sizeof(out[c].guard_types));
	}

	Bitboard all = occupied;
	while (all) {
		Square sq = all.pop();
		Piece pc = pos.piece_on(sq);
		Color c = color_of(pc);
		PieceType pt = type_of(pc);
		// 自駒のいる升も利きに含める (dlshogi の attack plane と同じ)。
		Bitboard atk = effects_from(pc, sq, occupied);
		out[c].type_atk[pt] |= atk;
		Bitboard t = atk;
		while (t)
			out[c].atk_count[(int)t.pop()]++;
	}

	for (Color c = BLACK; c < COLOR_NB; ++c)
		for (int i = 0; i < 14; ++i) {
			PieceType pt = kPlanePieceType[i];
			if (pt == KING) continue; // guard_types は玉の利きを数えない
			Bitboard t = out[c].type_atk[pt];
			while (t)
				out[c].guard_types[(int)t.pop()]++;
		}
}

// 玉 c 側の 50 次元 (features.py の _king_features と同じ順・同じ値)。
void king_features_one(const Position& pos, Color c, const SideAttacks* atk, double* out) {
	const Color      e        = ~c;
	const Square     ksq      = pos.square<KING>(c);
	const Bitboard   occupied = pos.pieces();
	const SideAttacks& own    = atk[c];
	const SideAttacks& enemy  = atk[e];

	// 玉を中心としたチェビシェフ距離 1 / 2 の輪。玉の升は含まない。
	Bitboard ring[2] = { Bitboard(0), Bitboard(0) };
	for (Square s = SQ_ZERO; s < SQ_NB; ++s) {
		int d = dist(ksq, s);
		if (d == 1)      ring[0] |= Bitboard(s);
		else if (d == 2) ring[1] |= Bitboard(s);
	}

	int n = 0;
	out[n++] = (enemy.atk_count[(int)ksq] >= 1) ? 1.0 : 0.0; // in_check
	out[n++] = (double)ring[0].pop_count();                  // ring1_size
	out[n++] = (double)ring[1].pop_count();                  // ring2_size

	const Bitboard own_pawns   = pos.pieces(c, PAWN);
	const Bitboard own_golds   = pos.pieces(c, GOLD, PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER);
	const Bitboard own_silvers = pos.pieces(c, SILVER);
	const Bitboard enemy_occ   = pos.pieces(e);

	for (int r = 0; r < 2; ++r) {
		const Bitboard mask = ring[r];

		out[n++] = (double)(own_pawns   & mask).pop_count();
		out[n++] = (double)(own_golds   & mask).pop_count();
		out[n++] = (double)(own_silvers & mask).pop_count();
		out[n++] = (double)(enemy_occ   & mask).pop_count();

		int guard_ge[3] = {0, 0, 0};
		int atk_ge[3]   = {0, 0, 0};
		int pressured_empty = 0, unpressured_empty = 0, bare_attacked = 0;

		Bitboard t = mask;
		while (t) {
			Square s = t.pop();
			const int g = own.guard_types[(int)s];
			const int a = enemy.atk_count[(int)s];
			for (int k = 0; k < 3; ++k) {
				if (g >= k + 1) guard_ge[k]++;
				if (a >= k + 1) atk_ge[k]++;
			}
			const bool is_empty    = !occupied.test(s);
			const bool is_attacked = (a >= 1);
			if (is_empty && is_attacked)   pressured_empty++;
			if (is_empty && !is_attacked)  unpressured_empty++;
			if (is_attacked && g == 0)     bare_attacked++;
		}

		out[n++] = (double)guard_ge[0];
		out[n++] = (double)guard_ge[1];
		out[n++] = (double)guard_ge[2];
		out[n++] = (double)atk_ge[0];
		out[n++] = (double)atk_ge[1];
		out[n++] = (double)atk_ge[2];
		out[n++] = (double)pressured_empty;
		out[n++] = (double)unpressured_empty;
		out[n++] = (double)bare_attacked;
	}

	// ring1 に利かせている相手の駒種ごとの升数 (PIECE_NAMES の順)。
	for (int i = 0; i < 14; ++i)
		out[n++] = (double)(enemy.type_atk[kPlanePieceType[i]] & ring[0]).pop_count();

	// 相手の持ち駒 × 露出度。合法な打ち場所の数でも詰みの印でもなく、ただの非線形項。
	int exposed = 0;
	{
		Bitboard t = ring[0];
		while (t) {
			Square s = t.pop();
			if (!occupied.test(s) && own.guard_types[(int)s] == 0)
				exposed++;
		}
	}
	const double exposure = (double)exposed / 8.0;
	const Hand eh = pos.hand_of(e);
	for (int i = 0; i < 7; ++i)
		out[n++] = (double)hand_count(eh, kHandOrder[i]) * exposure;

	ASSERT_LV3(n == NUM_KING_FEATURES);
}

} // namespace

void extract_material_features(const Position& pos, const Layout& layout, float* feat) {
	for (int i = 0; i < layout.material; ++i) feat[i] = 0.0f;
	static const int folded[PIECE_TYPE_NB] = {-1,0,1,2,3,5,6,4,-1,7,7,7,7,8,9,-1};
	static const int split[PIECE_TYPE_NB] = {-1,0,1,2,3,5,6,4,-1,7,8,9,10,11,12,-1};
	const int* slots = layout.pieces == Pieces::Folded ? folded : split;
	Bitboard all = pos.pieces();
	while (all) {
		Square sq = all.pop(); Piece pc = pos.piece_on(sq); int slot = slots[type_of(pc)];
		if (slot >= 0) feat[slot] += color_of(pc) == BLACK ? 1.0f : -1.0f;
	}
	const int hand_at = layout.pieces == Pieces::Folded ? 10 : 13;
	for (int i = 0; i < 7; ++i)
		feat[hand_at + i] = (float)(hand_count(pos.hand_of(BLACK), kHandOrder[i])
		                              - hand_count(pos.hand_of(WHITE), kHandOrder[i]));
}

void extract_king_features(const Position& pos, float* feat) {
	SideAttacks atk[COLOR_NB];
	build_side_attacks(pos, atk);

	double black[NUM_KING_FEATURES];
	double white[NUM_KING_FEATURES];
	king_features_one(pos, BLACK, atk, black);
	king_features_one(pos, WHITE, atk, white);

	// 先手視点 = 先手玉まわりの値 − 後手玉まわりの値。
	// features.py は手番側 − 相手側なので、後手番の局面では符号が逆になる。
	for (int i = 0; i < NUM_KING_FEATURES; ++i)
		feat[i] = (float)(black[i] - white[i]);
}

void extract_features_v2(const Position& pos, const Layout& layout, float* feat) {
	extract_material_features(pos, layout, feat + OFF_MATERIAL);
	extract_features(pos, layout, feat + layout.off_mobility());
	extract_king_features(pos, feat + layout.off_king());
}

float game_phase(const Position& pos) {
	int sum = 0;
	Bitboard all = pos.pieces();
	while (all) {
		Square sq = all.pop();
		sum += kPhaseWeight[type_of(pos.piece_on(sq))];
	}
	// -ffast-math で 82.0f/82.0f が 0.99999994 になり、平手初期局面の t が
	// 0 にならないことがある。引き算を先に行って double で割ると、
	// 盤上が減っていないとき厳密に 0 になる。
	double t = (double)(PHASE_MAX_MATERIAL - sum) / (double)PHASE_MAX_MATERIAL;
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	return (float)t;
}

void extract_features_phased(const Position& pos, const Layout& layout, float* feat) {
	const int n = layout.features();
	std::vector<float> x((size_t)n);
	extract_features_v2(pos, layout, x.data());
	const float t = game_phase(pos);
	for (int i = 0; i < n; ++i) {
		feat[i]     = x[i] * (1.0f - t);
		feat[n + i] = x[i] * t;
	}
}

double linear_value(const Position& pos, const Layout& layout, int kind, const float* w) {
	if (kind == 0)
		return mobility_dot(pos, layout, w, nullptr, 0.0f);

	const int   n = layout.features();
	const float t = (kind == 2) ? game_phase(pos) : 0.0f;

	float mat[MAX_MATERIAL_FEATURES];
	float kng[NUM_KING_FEATURES];
	extract_material_features(pos, layout, mat);
	extract_king_features(pos, kng);

	// 按分した並びでは、i 列目の実効的な重みは w[i]*(1-t) + w[n+i]*t。
	auto weight = [&](int i) -> double {
		return (kind == 2) ? ((double)w[i] * (1.0 - (double)t) + (double)w[n + i] * (double)t)
		                   : (double)w[i];
	};

	double sum = 0.0;
	for (int i = 0; i < layout.material; ++i)
		sum += weight(OFF_MATERIAL + i) * (double)mat[i];
	for (int i = 0; i < NUM_KING_FEATURES; ++i)
		sum += weight(layout.off_king() + i) * (double)kng[i];
	sum += mobility_dot(pos, layout, w + layout.off_mobility(),
	                    (kind == 2) ? w + n + layout.off_mobility() : nullptr, t);
	return sum;
}

namespace {

// 利きの列の名前。土台の 164 列は init_index_tables が控えた名前を引き、
// 掛けた軸を後ろに足す。番号の意味は manaka-hce-fit.rs の index_tables() が
// 持っているので、並びを変えたら向こうと突き合わせること。
std::string base_slot_name(const Layout& layout, int base) {
	if (!g_index_inited.load(std::memory_order_acquire)) init_index_tables();
	if (base < 0 || base >= layout.board + NUM_HAND_FEATURES) return std::to_string(base);
	return base < layout.board ? g_board_slot_name[base] : g_hand_slot_name[base - layout.board];
}

std::string mobility_slot_name(const Layout& layout, int m) {
	static const char* folded_names[10] = {"pawn","lance","knight","silver","bishop","rook","gold","king","horse","dragon"};
	static const char* split_names[14] = {"pawn","lance","knight","silver","bishop","rook","gold","king",
		"promoted_pawn","promoted_lance","promoted_knight","promoted_silver","horse","dragon"};
	const char* const* names = layout.pieces == Pieces::Folded ? folded_names : split_names;
	auto contact = [&](int k) {
		if (k == layout.contact_pieces * 2) return std::string(":empty");
		return std::string(k < layout.contact_pieces ? ":own." : ":enemy.") + names[k % layout.contact_pieces];
	};
	if (layout.contact && m >= layout.off_contact_hand) {
		int k=m-layout.off_contact_hand; return base_slot_name(layout, layout.board+k/layout.contact_dim)+contact(k%layout.contact_dim);
	}
	if (layout.contact && m >= layout.off_contact_board) {
		int k=m-layout.off_contact_board; return base_slot_name(layout,k/layout.contact_dim)+contact(k%layout.contact_dim);
	}
	if (layout.cross && m < layout.board*layout.cross) {
		int at=m/layout.cross, axis=m%layout.cross;
		return base_slot_name(layout,at)+(layout.cross==9?"@rank":"@sq")+std::to_string(axis);
	}
	if (!layout.contact && m >= layout.off_hand) return base_slot_name(layout,layout.board+m-layout.off_hand);
	return base_slot_name(layout,m);
}

} // anonymous namespace

const char* feature_v2_name(const Layout& layout, int index) {
	static const char* folded_material[17] = {"material.board.pawn","material.board.lance","material.board.knight","material.board.silver","material.board.gold","material.board.bishop","material.board.rook","material.board.promoted_minor","material.board.horse","material.board.dragon","material.hand.pawn","material.hand.lance","material.hand.knight","material.hand.silver","material.hand.gold","material.hand.bishop","material.hand.rook"};
	static const char* split_material[20] = {"material.board.pawn","material.board.lance","material.board.knight","material.board.silver","material.board.gold","material.board.bishop","material.board.rook","material.board.promoted_pawn","material.board.promoted_lance","material.board.promoted_knight","material.board.promoted_silver","material.board.horse","material.board.dragon","material.hand.pawn","material.hand.lance","material.hand.knight","material.hand.silver","material.hand.gold","material.hand.bishop","material.hand.rook"};
	static const char* kRingNames[13] = {
		"own_pawns", "own_golds", "own_silvers", "enemy_occupied",
		"own_guard_types_ge1", "own_guard_types_ge2", "own_guard_types_ge3",
		"enemy_attack_ge1", "enemy_attack_ge2", "enemy_attack_ge3",
		"pressured_empty", "unpressured_empty", "attacked_without_nonking_guard",
	};
	static const char* kPieceNames[14] = {
		"pawn", "lance", "knight", "silver", "bishop", "rook", "gold", "king",
		"promoted_pawn", "promoted_lance", "promoted_knight", "promoted_silver",
		"horse", "dragon",
	};
	static const char* kHandNames[7] = {
		"pawn", "lance", "knight", "silver", "gold", "bishop", "rook",
	};
	static std::string buf;

	if (index < 0 || index >= layout.features()) return "?";
	if (index < layout.off_mobility()) return layout.pieces == Pieces::Folded ? folded_material[index] : split_material[index];
	if (index < layout.off_king()) {
		buf = "mobility." + mobility_slot_name(layout, index - layout.off_mobility());
		return buf.c_str();
	}
	int k = index - layout.off_king();
	if (k == 0) return "king.in_check";
	if (k == 1) return "king.ring1_size";
	if (k == 2) return "king.ring2_size";
	k -= 3;
	if (k < 26) {
		buf = "king.ring" + std::to_string(k / 13 + 1) + "." + kRingNames[k % 13];
		return buf.c_str();
	}
	k -= 26;
	if (k < 14) {
		buf = std::string("king.ring1.enemy_attacks_by.") + kPieceNames[k];
		return buf.c_str();
	}
	k -= 14;
	buf = std::string("king.enemy_hand_times_exposure.") + kHandNames[k];
	return buf.c_str();
}

} // namespace HumanLike
} // namespace Eval
} // namespace YaneuraOu

// ---------------------------------------------------------------------------
// mobility_dump.cpp は source/Makefile の SOURCES に入っていない (Makefile は
// 今回の担当範囲外なので足せない)。同じ translation unit に取り込んで生かす。
// ---------------------------------------------------------------------------
#define HCE_DUMP_INCLUDED_FROM_HUMANLIKE_EVAL 1
#include "mobility_dump.cpp"
