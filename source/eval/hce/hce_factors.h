#ifndef HCE_FACTORS_H_INCLUDED
#define HCE_FACTORS_H_INCLUDED

#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include <cstdint>
#include <vector>

#include "hce_board.h"
#include "hce_net.h"

namespace YaneuraOu {
namespace Eval {
namespace Hce {

// 玉の安全度と駒の働きの入力。どちらも hce_board の升ごとの事実から作る。
// 並びと値は scripts/train_hce_e2e.py の king_window と piece_facts と同じ。

// 玉の周りの窓の 1 マスあたりの値の数。駒の種類 29 (空き、自駒 14、敵駒 14) の one-hot、
// 自分の利きの枚数 / 4、敵の利きの枚数 / 4、盤外フラグ。
constexpr int WINDOW_CELL = 32;

// 窓の幅。半径 r なら (2r + 1)^2 マスに、相手の持ち駒 7 つを足したもの。
inline int window_width(int radius) { return (2 * radius + 1) * (2 * radius + 1) * WINDOW_CELL + 7; }

// colour (0 先手、1 後手) の玉の窓。out は window_width(radius) 個。
void king_window(const uint8_t* board, int colour, int radius, float* out);

// 駒ごとの事実の数。
constexpr int PIECE_FACTS = 14 + 3 + 10 + 2 + 1 + 9 + 9 + 9;

// 盤上の駒 (玉を除く) の事実を並べる。facts は PIECE_FACTS 個ずつ、sign は先手 +1、後手 −1。
// 戻り値は駒の数。
int piece_facts(const uint8_t* board, std::vector<float>& facts, std::vector<float>& sign);

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
#endif // HCE_FACTORS_H_INCLUDED
