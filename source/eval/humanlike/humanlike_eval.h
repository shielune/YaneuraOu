#ifndef _HUMANLIKE_EVAL_H_INCLUDED_
#define _HUMANLIKE_EVAL_H_INCLUDED_

#include <sstream>
#include <string>
#include <vector>

#include "../../types.h"

namespace YaneuraOu {

class Position;

namespace Eval {
namespace HumanLike {

// 評価関数モード (humanlike_eval 名前空間に常駐するハンドクラフト/小型線形 eval)
//
//   Nnue    : 既存 NNUE (KP256 など、ビルドの YANEURAOU_EDITION で決定)
//   MT      : Material — YaneuraOu 標準 PieceValue による駒得合計 (学習なし)
//   MB      : Mobility (learned) — 駒種×距離×方向 (164 params) を hao_depth9 に Ridge fit
//   KPL     : K+P 線形 (1,710 params) — BonaPiece (玉 162 + 駒 1548) の active を線形加算
//   HalfKPL : HalfKP cross-product 線形 (125,388 params) — 玉位置 × 任意 1 駒、Friend - Enemy
//
// 直交軸として ForceCapture (FC) フラグがあるが、これは USI option
// で管理 (このモードリストとは独立):
//   ForceCapture (FC) : 探索の全ノード × 自分の手番で free capture (タダ取り) を強制。
//                       per-side per-node filter なので計画と実プレイが完全一致する。
enum class Mode {
	Nnue,
	Material,   // USI 文字列: "MT"
	Mobility,   // USI 文字列: "MB" — 学習済 164-dim 線形評価
	KPL,        // USI 文字列: "KPL" — 1,710-dim 線形評価
	HalfKPL,    // USI 文字列: "HalfKPL" — 125,388-dim 線形評価 (Friend-Enemy)
};

// EvalMode option の選択肢 (Combo 用)。先頭は default の "nnue"。
extern const std::vector<std::string> mode_names;

Mode parse_mode(const std::string& s);

Value evaluate(const Position& pos, Mode mode, bool capture_force);

// move を指すと相手駒を取り、移動先升での自分の利き枚数 > 相手の利き枚数 のとき true。
// 「枚数優位な升への捕獲手」=「タダ取り」とみなす (CP/SEE 判定は意図的に行わない)。
bool is_free_capture(const Position& pos, Move m);

// MB 用 feature 表現。
//
//   [  0, 107) 盤上の駒の利き     … 駒種 × 距離 × 方向
//   [107, 164) 持ち駒を打った利き … 合法な打ち場所すべての利きの合計
//
// この 164 列を土台にして、次の三つの軸を掛けた版を作る。
//
//   段 … 利かせている駒が自分から見て何段目に居るか (9 通り)
//   升 … 利かせている駒が自分から見てどの升に居るか (81 通り)
//   先 … 利きの指す升に何が居るか (自分の駒 10 種 + 相手の駒 10 種 + 空白 = 21 通り)
//
// 掛けた列は元の列を置き換える。分けたものを足すと元の列に戻るので、両方を
// 並べると従属な列が増えるだけになる。段と升は盤上の 107 列にだけ掛け、
// 先は盤上と持ち駒の両方に掛ける (持ち駒を打った利きも升を指しているので)。
//
//   plain           107 + 57                =   164
//   rank            107*9 + 57              =  1020
//   square          107*81 + 57             =  8724
//   contact         (107 + 57)*21           =  3444
//   rank+contact    107*9 + (107 + 57)*21   =  4407
//   square+contact  107*81 + (107 + 57)*21  = 12111
//
// 後ろの二つは段 (升) の版と先の版を横に並べたもので、三重には掛けない。
// 107*81*21 は 18 万列あり、Gram 行列が 265 GB になって解けない。
//
// 駒種は 歩 香 桂 銀 角 飛 金 玉 馬 龍 の 10 個。と金・成香・成桂・成銀は動きが
// 金と同じなので金の列に入れる。盤上 107 列が使う駒種の分け方と同じ。
//
// 先の版で持ち駒の 57 列を分けるのは、打った先の升に何が居るかで利きの意味が
// 変わるため。打つ升そのものは空でなければならないが、そこから伸びる利きの先は
// 空とは限らない。
constexpr int NUM_BOARD_FEATURES_FOLDED = 107;
constexpr int NUM_BOARD_FEATURES_SPLIT  = 123;
constexpr int MAX_BOARD_FEATURES        = NUM_BOARD_FEATURES_SPLIT;
constexpr int NUM_HAND_FEATURES         = 57;
// Compatibility names for the embedded folded-only evaluator.
constexpr int NUM_BOARD_FEATURES = NUM_BOARD_FEATURES_FOLDED;
constexpr int NUM_PLAIN_MOBILITY = NUM_BOARD_FEATURES_FOLDED + NUM_HAND_FEATURES;
constexpr int NUM_CONTACT_PIECES_FOLDED = 10;
constexpr int NUM_CONTACT_PIECES_SPLIT  = 14;
constexpr int NUM_MATERIAL_FOLDED       = 17;
constexpr int NUM_MATERIAL_SPLIT        = 20;
constexpr int MAX_MATERIAL_FEATURES     = NUM_MATERIAL_SPLIT;
constexpr int NUM_KING_FEATURES         = 50;
// 手番の 2 列。先手番なら +1、後手番なら -1 を s として、s と s*t (t は進行度)。
// 他の列と同じく先手視点なので、正の重みは指す側の得になる。
constexpr int NUM_TEMPO_FEATURES        = 2;
constexpr int OFF_MATERIAL              = 0;

enum class Pieces { Folded, Split };
const char* pieces_name(Pieces p);
bool parse_pieces(const std::string& s, Pieces& out);

enum class Variant { Plain, Rank, Square, Contact, RankContact, SquareContact };
constexpr int NUM_VARIANTS = 6;
const char* variant_name(Variant v);
bool parse_variant(const std::string& s, Variant& out);

struct Layout {
	Variant variant = Variant::Plain;
	Pieces  pieces = Pieces::Folded;
	int cross = 0;
	bool contact = false;
	int board = NUM_BOARD_FEATURES_FOLDED;
	int contact_pieces = NUM_CONTACT_PIECES_FOLDED;
	int contact_dim = 21;
	int material = NUM_MATERIAL_FOLDED;
	int off_cross = 0;
	int off_contact_board = 0;
	int off_contact_hand = 0;
	int off_hand = NUM_BOARD_FEATURES_FOLDED;
	int mobility = NUM_BOARD_FEATURES_FOLDED + NUM_HAND_FEATURES;

	int off_mobility() const { return material; }
	int off_king() const { return material + mobility; }
	int features() const { return material + mobility + NUM_KING_FEATURES; }
	int tapered() const { return features() * 2; }
	int with_tempo() const { return features() + NUM_TEMPO_FEATURES; }
};

Layout make_layout(Variant v, Pieces p = Pieces::Folded);
bool layout_for_dim(int dim, Layout& out, int& kind);

constexpr int MAX_MOBILITY_FEATURES =
    NUM_BOARD_FEATURES_SPLIT * 81
    + (NUM_BOARD_FEATURES_SPLIT + NUM_HAND_FEATURES) * (NUM_CONTACT_PIECES_SPLIT * 2 + 1);
constexpr int MAX_FEATURES_V2 = MAX_MATERIAL_FEATURES + MAX_MOBILITY_FEATURES + NUM_KING_FEATURES;
constexpr int MAX_FEATURES_V2_PHASED = MAX_FEATURES_V2 * 2;

inline int contact_piece_index(PieceType pt, Pieces pieces) {
	if (pieces == Pieces::Split) {
		switch (pt) {
		case PAWN:return 0; case LANCE:return 1; case KNIGHT:return 2; case SILVER:return 3;
		case BISHOP:return 4; case ROOK:return 5; case GOLD:return 6; case KING:return 7;
		case PRO_PAWN:return 8; case PRO_LANCE:return 9; case PRO_KNIGHT:return 10;
		case PRO_SILVER:return 11; case HORSE:return 12; case DRAGON:return 13;
		default:return 0;
		}
	}
	switch (pt) {
	case PAWN:return 0; case LANCE:return 1; case KNIGHT:return 2; case SILVER:return 3;
	case BISHOP:return 4; case ROOK:return 5; case GOLD: case PRO_PAWN: case PRO_LANCE:
	case PRO_KNIGHT: case PRO_SILVER:return 6; case KING:return 7; case HORSE:return 8;
	case DRAGON:return 9; default:return 0;
	}
}

// 現在位置の利き列を feat に書き込む (先手視点 +、後手視点 -)。
// 幅は layout.mobility。plain なら 164 で、以前の 164 次元とそのまま一致する。
void extract_features(const Position& pos, const Layout& layout, float* feat);

// 利き列と重みの内積。feature ベクトルを作らずに利きを走査しながら足す。
// 探索の各節点で 12,111 個の float を 0 で埋めるのは現実的でないので、線形の
// 評価はこちらを通す。w1 が nullptr でないときは w0[i]*(1-t) + w1[i]*t を重みと
// して使う (進行度で按分した並び)。
double mobility_dot(const Position& pos, const Layout& layout,
                    const float* w0, const float* w1, float t);

// 重みとの内積そのもの (先手視点、切片の前)。kind は layout_for_dim が返すもので、
// 0 なら利きだけ、1 なら駒得と玉の安全度も、2 は進行度で按分した並び、
// 3 は 1 の後ろに手番の 2 列 (layout.with_tempo() 列)。
// 利きの部分は mobility_dot を通るので、幅の広い版でも配列を作らない。
double linear_value(const Position& pos, const Layout& layout, int kind, const float* w);

// MobilityWeightsFile USI option から呼ばれる。plain の 164 個の float を読む。
bool load_mobility_weights(const std::string& path);

// USI コマンド: binpack から Mobility feature を抽出して X.bin / y.bin を出力する。
void mobility_dump_cmd(Position& pos, std::istringstream& is);

// USI コマンド: binpack の各 record で sfen ↔ score の整合性を診断する。
// 各 record で (1) static eval, (2) qsearch eval を計算し、record.score と比較。
// sfen が qsearch leaf に置換済なら static eval ≈ score、root のままなら乖離する。
void qs_consistency_cmd(Position& pos, std::istringstream& is);

// ---------------------------------------------------------------------------
// KPL: K+P linear (1,710 params)
// ---------------------------------------------------------------------------
// 入力レイアウト (model/features/blocks.py の KP ブロックと一致):
//   index ∈ [0, 1548)        … P (玉以外の BonaPiece、pieces[i] そのまま)
//   index ∈ [1548, 1710)     … K (pieces[i] - fe_end + 1548)
// active 数は 38 (P) + 2 (K) = 40。
constexpr int NUM_KPL_P_FEATURES = 1548; // = fe_end (玉以外)
constexpr int NUM_KPL_K_FEATURES = 81 * 2; // 玉 162 (先手 + 後手)
constexpr int NUM_KPL_FEATURES   = NUM_KPL_P_FEATURES + NUM_KPL_K_FEATURES; // 1710

// 現在位置の KPL active indices (40 個) を idx_out に書き込み、active 数を返す。
// 先手 POV (perspective=BLACK 視点の BonaPiece リストを使用)。
int extract_kpl_indices(const Position& pos, int* idx_out);

// KPLWeightsFile USI option から呼ばれる。1710 個の float を読み込む。
bool load_kpl_weights(const std::string& path);

// USI コマンド: binpack から KPL sparse feature を抽出して .idx.bin / .y.bin を出力。
void kpl_dump_cmd(Position& pos, std::istringstream& is);

// ---------------------------------------------------------------------------
// HalfKPL: HalfKP cross-product linear (125,388 params)
// ---------------------------------------------------------------------------
// 入力レイアウト (model/features/blocks.py の HalfKP ブロックと一致):
//   index = king_sq * 1548 + p_bonapiece    ∈ [0, 125388)
// 評価式は Friend - Enemy:
//   score = Σ_i  w[k_BLACK_anchor * 1548 + pieces_fb[i]]
//         − Σ_i  w[k_WHITE_anchor * 1548 + pieces_fw[i]]
//   ( i: 玉以外の 38 駒 )
// active 数は 38 (Friend) + 38 (Enemy) = 76。
constexpr int NUM_HKPL_FE_END   = 1548;
constexpr int NUM_HKPL_KING_SQ  = 81;
constexpr int NUM_HKPL_FEATURES = NUM_HKPL_KING_SQ * NUM_HKPL_FE_END; // 125388

// HalfKPL active indices を Friend 側 38 個と Enemy 側 38 個に分けて書き込む。
// 引数の配列はそれぞれ 38 個以上のサイズを持つこと。
void extract_halfkpl_indices(const Position& pos,
                              int* friend_out, int* enemy_out);

// HalfKPLWeightsFile USI option から呼ばれる。125388 個の float を読み込む。
bool load_halfkpl_weights(const std::string& path);

// USI コマンド: binpack から HalfKPL sparse feature を抽出して dump する。
void halfkpl_dump_cmd(Position& pos, std::istringstream& is);

// ---------------------------------------------------------------------------
// HCE v2 (docs/ja/manaka/hce-v2.md)
// ---------------------------------------------------------------------------
// 並び (plain なら 231 次元):
//   [  0,  17) 駒得        … 盤上 10 種 + 持ち駒 7 種 (manaka-hce-fit.rs の material_board_slot)
//   [ 17,   *) 利き        … layout.mobility 列。先頭 164 の並びを動かしてはいけない
//   [  *,   *) 玉の安全度  … tideborn/king_safety/features.py の KING_NAMES と同じ順
//
// 利きの幅は版で変わる (Layout を見ること)。駒得と玉の安全度の幅は変わらない。
// すべて先手番から見た値 (先手の値 − 後手の値)。手番による符号反転は呼び出し側が行う。

// 駒得 17 次元 (先手視点)。盤上 pawn,lance,knight,silver,gold,bishop,rook,
// promoted_minor,horse,dragon の 10 個に続けて、持ち駒を
// pawn,lance,knight,silver,gold,bishop,rook の順で 7 個。
// (持ち駒の並びは cshogi の pieces_in_hand と同じで、やねうら王の PieceType 順ではない)
void extract_material_features(const Position& pos, const Layout& layout, float* feat);

// 玉の安全度 50 次元 (先手視点 = 先手玉まわりの値 − 後手玉まわりの値)。
void extract_king_features(const Position& pos, float* feat);

// 駒得・利き・玉の安全度をまとめて書き込む (layout.features() 列)。
void extract_features_v2(const Position& pos, const Layout& layout, float* feat);

// 進行度 t。盤上の駒 (玉を除く) に 歩1 香2 桂2 銀3 金4 角5 飛5 を与えて合計し、
// 成り駒は成る前の値で数える。持ち駒は数えない。t = clamp(1 - 合計/82, 0, 1)。
constexpr int PHASE_MAX_MATERIAL = 82;
float game_phase(const Position& pos);

// 進行度で按分した並び (layout.tapered() 列)。
void extract_features_phased(const Position& pos, const Layout& layout, float* feat);

// 手番の 2 列の値 (s と s*t)。kind 3 の重みの末尾 2 個に掛ける。
void extract_tempo_features(const Position& pos, float* z);

// 各列の名前 (dump の見出しと突き合わせ用)。
const char* feature_v2_name(const Layout& layout, int index);

// 手番の列の名前。k は 0 か 1。manaka-hce-fit.rs の TEMPO_NAMES と同じ。
const char* tempo_feature_name(int k);

// ---------------------------------------------------------------------------
// dump (mobility_dump.cpp)
// ---------------------------------------------------------------------------
// Rust 側の `manaka-hce-fit dump` と突き合わせるためのテキスト出力。
// 1 局面につき sfen / mat / want / king / featv2 / phase の 6 行を書く。
void hce_dump_position(std::ostream& os, const Position& pos, const Layout& layout,
                       const std::string& sfen);
void hce_dump_sfen(std::ostream& os, const Layout& layout, const std::string& sfen);

// USI option 経由で呼ばれる入口。args は "input ... output_prefix ... dims ..." 形式。
void hce_dump_file_cmd(const std::string& args);      // SFEN を 1 行 1 局面で読む
void hce_dump_binpack_cmd(const std::string& args);   // PackedSfenValue (40B/record) を読む
void hce_make_binpack_cmd(const std::string& args);   // sfen 一覧から PackedSfenValue を作る (試験用)

} // namespace HumanLike
} // namespace Eval
} // namespace YaneuraOu

#endif
