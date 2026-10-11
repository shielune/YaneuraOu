#ifndef HCE_BOARD_H_INCLUDED
#define HCE_BOARD_H_INCLUDED

#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include <cstdint>

#include "../../types.h"

namespace YaneuraOu {

class Position;

namespace Eval {
namespace Hce {

// 局面を升ごとの事実に直したもの。explain の玉の安全度と駒の働きの入力は、ここから作る。
//
// 1 升に 12 個の値があり、並びは学習側の hce_board.rs と同じ。
//   0       その升の駒。0 は空き、あとは 1 + 種類 + 14 x 色 (種類は K R B G S N L P +R +B +S +N +L +P、
//           色は先手 0、後手 1)
//   1, 2    その升に利きを持つ先手、後手の駒の枚数
//   3, 4    利きを持つ先手の駒の種類の集合 (10 種を 10 ビットで。下位 8 ビット、上位 2 ビット)
//   5, 6    同じく後手
//   7       その升にいる駒が、空きの升に利かせている数
//   8       自分の側の駒に利かせている数
//   9       相手の側の駒に利かせている数
//   10, 11  利かせている相手の駒の種類の集合 (下位 8 ビット、上位 2 ビット)
// そのあとに先手の持ち駒、後手の持ち駒 (歩 香 桂 銀 金 角 飛)、手番 (先手 0、後手 1)。
//
// 升の番号は学習側のもの、つまり (9 - 筋) x 9 + (段 - 1)。エンジンの (筋 - 1) x 9 + (段 - 1) とは
// 筋が逆向きなので、変換して並べ直す。
constexpr int PER_SQUARE = 12;
constexpr int NUM_BOARD = 81 * PER_SQUARE + 15;

// out は NUM_BOARD 個。
void board_features(const Position& pos, uint8_t* out);

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
#endif // HCE_BOARD_H_INCLUDED
