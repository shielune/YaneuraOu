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
		if (slot < 0) slot = idx++;
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
		for (auto [df, dr] : gold_offs) {
			add_board(GOLD, df, dr);
			int gid = g_board_idx_table[GOLD][std::abs(df) + OFFSET_BIAS][dr + OFFSET_BIAS];
			for (PieceType pt : {PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER})
				alias_to(pt, df, dr, gid);
		}
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

	if (idx != NUM_BOARD_FEATURES) {
		std::cerr << "humanlike_eval: board index count mismatch: " << idx
		          << " vs " << NUM_BOARD_FEATURES << std::endl;
		std::abort();
	}

	auto add_hand = [&](PieceType pt, int df, int dr) {
		df = std::abs(df);
		int& slot = g_hand_idx_table[pt][df + OFFSET_BIAS][dr + OFFSET_BIAS];
		if (slot < 0) slot = idx++;
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

	if (idx != NUM_FEATURES) {
		std::cerr << "humanlike_eval: total index count mismatch: " << idx
		          << " vs " << NUM_FEATURES << std::endl;
		std::abort();
	}

	g_index_inited.store(true, std::memory_order_release);
}

// MB の学習済 weight。default 0 → 評価値 0 (loadなしだと中立)。
float g_mobility_weights[NUM_FEATURES] = {0};
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

// Piece (0..31) を 0..20 の正規化駒種インデックスに変換する。
//   0        : NO_PIECE (空升)
//   1..10    : 先手駒 (B_PAWN=1 .. B_DRAGON=10)
//   11..20   : 後手駒 (W_PAWN=11 .. W_DRAGON=20)
// 先手・後手の Piece 値は PIECE_TYPE_NB (= 16) 刻みでオフセットされているため、
// type_of() で駒種 (1..15) を取り出し、color で先後オフセットを足す。
inline int normalize_piece(Piece pc) {
	if (pc == NO_PIECE) return 0;
	int pt = (int)type_of(pc); // 1..15 (PAWN..DRAGON, PRO_* を含む)
	// PRO_* (9..15) を基本駒種 (1..8) にまとめる: PIECE_TYPE_NB/2 = 8 を上限として折り返す
	if (pt > 8) pt -= 8; // PRO_PAWN(9)→1, ..., DRAGON(14)→6, KING(15)→7... ただし HORSE/DRAGON は別
	// 実際の駒種は PAWN(1)〜DRAGON(14)、KING(15)。10種に収める。
	// → type_of() の値を 1..10 の範囲に収める素直なマッピング:
	//   PAWN=1,LANCE=2,KNIGHT=3,SILVER=4,BISHOP=5,ROOK=6,GOLD=7,KING=8,HORSE=9,DRAGON=10
	//   PRO_PAWN〜PRO_SILVER は GOLD(7) に統一 (盤上特徴量と同じ規則)
	static const int piece_map[16] = {
		0,  // NO_PIECE_TYPE (使わない)
		1,  // PAWN
		2,  // LANCE
		3,  // KNIGHT
		4,  // SILVER
		5,  // BISHOP
		6,  // ROOK
		7,  // GOLD
		8,  // KING
		9,  // HORSE
		10, // DRAGON
		7,  // PRO_PAWN  → GOLD 扱い
		7,  // PRO_LANCE → GOLD 扱い
		7,  // PRO_KNIGHT→ GOLD 扱い
		7,  // PRO_SILVER→ GOLD 扱い
		0,  // (unused slot 15)
	};
	int base = piece_map[(int)type_of(pc)]; // 1..10
	return (color_of(pc) == BLACK) ? base : base + 10; // 先手1..10, 後手11..20
}

// MOBILITY_VARIANT 別の feat[] index 計算。base_idx は g_*_idx_table から得た 0..163 の値。
// sq は攻撃駒の升 (持ち駒の場合は打ち込み升)。target_pc は利き先の駒。
inline int feat_index(int base_idx, Square sq, Piece target_pc) {
#if MOBILITY_VARIANT == 2
	return base_idx * NUM_TARGET_TYPES + normalize_piece(target_pc);
#elif MOBILITY_VARIANT == 3
	return base_idx * NUM_SQUARES + (int)sq;
#elif MOBILITY_VARIANT == 4
	return (base_idx * NUM_TARGET_TYPES + normalize_piece(target_pc)) * NUM_SQUARES + (int)sq;
#else
	(void)sq; (void)target_pc;
	return base_idx;
#endif
}

} // anonymous namespace

void extract_features(const Position& pos, float* feat) {
	if (!g_index_inited.load(std::memory_order_acquire))
		init_index_tables();

	for (int i = 0; i < NUM_FEATURES; ++i) feat[i] = 0.0f;

	const Bitboard occupied = pos.pieces();

	// 盤上駒
	Bitboard all = occupied;
	while (all) {
		Square sq = all.pop();
		Piece pc = pos.piece_on(sq);
		PieceType pt = type_of(pc);
		Color c = color_of(pc);
		float sign = (c == BLACK) ? 1.0f : -1.0f;
		Bitboard atk = effects_from(pc, sq, occupied);
		const int sf = (int)file_of(sq), sr = (int)rank_of(sq);
		while (atk) {
			Square ts = atk.pop();
			int df = (int)file_of(ts) - sf;
			int dr = (int)rank_of(ts) - sr;
			if (c == WHITE) dr = -dr;
			int base_idx = g_board_idx_table[pt][std::abs(df) + OFFSET_BIAS][dr + OFFSET_BIAS];
			if (base_idx >= 0)
				feat[feat_index(base_idx, sq, pos.piece_on(ts))] += sign;
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
					int base_idx = g_hand_idx_table[pt][std::abs(df) + OFFSET_BIAS][dr + OFFSET_BIAS];
					if (base_idx >= 0)
						feat[feat_index(base_idx, sq, pos.piece_on(ts))] += sign * (float)cnt;
				}
			}
		}
	}
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
	tmp.reserve(NUM_FEATURES);
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
	if (tmp.size() != NUM_FEATURES) {
		std::cerr << "humanlike_eval[MB]: " << tmp.size()
		          << " entries, expected " << NUM_FEATURES << std::endl;
		return false;
	}
	for (size_t i = 0; i < NUM_FEATURES; ++i) g_mobility_weights[i] = tmp[i];
	g_mobility_weights_loaded.store(true, std::memory_order_release);
	std::cerr << "humanlike_eval[MB]: loaded " << NUM_FEATURES << " weights from " << path << std::endl;
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
	float feat[NUM_FEATURES];
	extract_features(pos, feat);
	double sum = 0.0;
	for (int i = 0; i < NUM_FEATURES; ++i)
		sum += (double)g_mobility_weights[i] * (double)feat[i];
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

} // namespace HumanLike
} // namespace Eval
