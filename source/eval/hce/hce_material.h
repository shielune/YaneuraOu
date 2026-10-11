#ifndef HCE_MATERIAL_H_INCLUDED
#define HCE_MATERIAL_H_INCLUDED

#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include "../../types.h"

namespace YaneuraOu {

class Position;

namespace Eval {
namespace Hce {

// explain の「駒得」の材料。駒の種類ごとの、先手の枚数 − 後手の枚数。
//
// 17 列。盤上は 歩 香 桂 銀 金 角 飛 と金(成香・成桂・成銀を含む) 馬 龍、持ち駒は
// 歩 香 桂 銀 金 角 飛。玉は数えない。
// 探索の SEE が使う Eval::PieceValue とは別で、そちらには触らない。
constexpr int NUM_MATERIAL_COLUMNS = 17;

void material_counts(const Position& pos, int* count);

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
#endif // HCE_MATERIAL_H_INCLUDED
