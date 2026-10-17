#ifndef EVAL_MANAKA_EVALUATE_MANAKA_H_INCLUDED
#define EVAL_MANAKA_EVALUATE_MANAKA_H_INCLUDED

// Manaka 評価関数と YaneuraOu の接着部分の公開宣言。
// 約束事 (どのファイルを誰が持つか、StateInfo の枠、検査コマンドの名前) は
// eval/manaka/API.md に書いてある。ここの宣言を変えるときは API.md も直すこと。
//
// Eval:: 側の入口 (add_options, load_eval, compute_eval, evaluate,
// evaluate_with_no_return, print_eval_stat) は evaluate.h が宣言済みなので、
// ここでは宣言しない。ここにあるのは Manaka 固有の入口だけ。

#include "../../config.h"

#if defined(EVAL_MANAKA)

#include <iosfwd>

#include "../../types.h"

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

// trace_eval (USI の "eval" コマンド) が、"eval <値>" の行の前に呼ぶ。
// 1 行 "manaka pre <f32 を %.9g> internal <整数>" を sync_cout で出す。
// pre は内部値へ写す前の f32 の出力 (tanh の前)、internal は内部単位の整数。
// 骨組みでは評価が常に 0 なので "manaka pre 0 internal 0" を出す。
// Position は const で受ける。trace_eval は SFEN から新しい Position を作るので、
// ここへ来るのは常に全計算の経路になる (差分更新の検査には使えない)。
void trace(const Position& pos);

// "test manaka_walk games N plies M seed S"
// 差分更新と全計算の一致検査 (power sum と pair の和がビット単位で一致すること)。
// 骨組みでは未実装と言って返るだけ。
void test_walk(IEngine& engine, std::istringstream& is);

// "test manaka_bench <局面ファイル> <回数> [warm|cold]"
// 評価 1 回あたりの時間と、全計算と差分更新の回数を測る。
// 骨組みでは未実装と言って返るだけ。
void test_bench(IEngine& engine, std::istringstream& is);

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)

#endif // EVAL_MANAKA_EVALUATE_MANAKA_H_INCLUDED
