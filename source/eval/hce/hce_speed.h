#ifndef HCE_SPEED_H_INCLUDED
#define HCE_SPEED_H_INCLUDED

#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include "../../types.h"

namespace YaneuraOu {

class Position;

namespace Eval {
namespace Hce {

// explain の「速度」の材料。どちらの玉が先に詰まされそうかを、王手と短い詰みで見る。
//
// 攻める側を二通りにして、それぞれ 4 つの数を数える。
//   mover  : 今の手番の側が、そのまま攻める。
//   waiter : 手番を相手に渡したものとして、手番でない側が攻める。ここに詰みがあれば
//            手番側は詰めろを掛けられている。
//
// | 列 | 値 |
// |---|---|
// | checks      | 王手になる合法手の数 |
// | safe_checks | 王手のうち、王手をかけた駒のいる升に取り返す合法手が無いものの数 |
// | mate1       | 1 なら、王手をかけて相手に合法手が無くなる手がある (1 手詰め) |
// | mate3       | 1 なら mate1、または、どの応手に対しても 1 手詰めが残る王手がある (3 手詰め) |
//
// 列は 先手視点の符号付き: 攻める側が先手なら +、後手なら −。並びは
// mover の 4 つ、waiter の 4 つ。手番側が王手をかけられているときは手番を渡せない
// ので waiter の 4 つは 0。
//
// 打ち歩詰めは合法手ではないので、詰みには数えない。
constexpr int NUM_SPEED = 8;

extern const char* const kSpeedNames[NUM_SPEED];

// pos は合法手の生成と do_move/undo_move のため const でない。戻るときは元の局面に戻る。
void speed_features(Position& pos, int* out);

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
#endif // HCE_SPEED_H_INCLUDED
