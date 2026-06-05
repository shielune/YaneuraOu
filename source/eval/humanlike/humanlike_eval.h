#ifndef _HUMANLIKE_EVAL_H_INCLUDED_
#define _HUMANLIKE_EVAL_H_INCLUDED_

#include <sstream>
#include <string>
#include <vector>

#include "../../types.h"

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
constexpr int NUM_BOARD_FEATURES = 107;
constexpr int NUM_HAND_FEATURES  = 57;
constexpr int NUM_FEATURES       = NUM_BOARD_FEATURES + NUM_HAND_FEATURES;

// 現在位置の 164 次元 feature ベクトルを feat に書き込む (先手視点 +、後手視点 -)。
void extract_features(const Position& pos, float* feat);

// MobilityWeightsFile USI option から呼ばれる。164 個の float を読み込む。
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

} // namespace HumanLike
} // namespace Eval

#endif
