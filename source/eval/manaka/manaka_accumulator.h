#ifndef EVAL_MANAKA_MANAKA_ACCUMULATOR_H_INCLUDED
#define EVAL_MANAKA_MANAKA_ACCUMULATOR_H_INCLUDED

// Manaka 評価の差分計算用の枠。StateInfo の「do_move でコピーされない」領域に置く。
// position.h が include するので、YaneuraOu の型は types.h の Value だけを使い、
// 評価の中身は純 C++17 の core (core/q16.h) の型をそのまま抱える。
// 約束事 (不変条件、フラグの扱い) は eval/manaka/API.md に書いてある。

#include <cstdint>
#include <type_traits>

#include "../../config.h"
#include "../../types.h"

#if defined(EVAL_MANAKA)

#include "core/q16.h"

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

constexpr int kFrames    = 2;  // 先手枠と後手枠
constexpr int kMaxTokens = 40; // 1 枠あたりのトークン数。SYMq16 は 40 枚ちょうどの局面だけを評価する

// StateInfo が memcpy と memset で扱われる値型なので、可変長の所有物 (vector など) は持てない。
// core の Q16Accumulator は配列とポインタだけの値型で、そのまま代入でコピーできる。
// 2 つの状態 (core の型と、StateInfo 用の別の配置) を持たず、core の型を 1 つだけ持つ。
// 並びの違う型に memcpy しない。祖先から写すときは q16 メンバーを代入する。
//
// AVX 系の整列読み書きのために 64 バイト境界に置く (NNUE の Accumulator と同じ)。
struct alignas(64) Accumulator {
	// 枠ごとの power sum (P1、P2、P3)、pair の和、各枠の 40 トークン (昇順)。
	// 整数で厳密に持つので、差分更新の結果は全計算とビット単位で一致する。
	// 評価関数の形の上限 (dim128、order3、pair16、40 トークン) は core の型が固定している。
	ManakaCore::Q16Accumulator q16;

	// q16 がそろって有効か。更新の途中で false にし、そろってから最後に true にする。
	bool          computed_accumulation = false;

	// score が現局面の値か。手番に依存するので do_null_move() でも倒す。
	bool          computed_score = false;

	// 手番側から見た内部値。computed_score が真のときだけ意味を持つ。
	Value         score = VALUE_ZERO;

	// 上の値を作ったときの重みの世代。0 は未読み込み。
	// isready で重みが読み直されると世代が進み、古い世代の値は旗が真でも使わない
	// (rootState は setupStates->back() の丸ごとのコピーなので、読み直しをまたいで残りうる。
	// q16.network のポインタだけでは、同じアドレスに作り直された別の重みを見分けられない)。
	std::uint32_t weights_gen = 0;

	// q16 を作った (rebuild か update で書いた) スレッドの番号。0 は誰も作っていない。
	// 差分更新の親には、自分のスレッドが作ったものだけを使う。thread.cpp の rootState は
	// setupStates->back() の丸ごとのコピーで、探索スレッドから見て root 以前の StateInfo
	// (rootState の previous) は全スレッドで共有の読み取り専用なので、他のスレッドの印が付いている。
	// Eval::evaluate は Position しか受け取らず root の境界を知る手段が無いので、境界を位置ではなく
	// 作成者で表す。丸ごとのコピーでも印は引き継がれる (do_null_move は自分の状態のコピーなので自分の印のまま)。
	std::uint32_t owner = 0;
};

// StateInfo は memcpy (do_null_move) と memset (Position::set) で扱われる。
static_assert(std::is_trivially_copyable<Accumulator>::value, "Manaka::Accumulator must be trivially copyable");
static_assert(alignof(Accumulator) == 64, "Manaka::Accumulator must be 64-byte aligned");
static_assert(sizeof(Accumulator) % 64 == 0, "Manaka::Accumulator size must be a multiple of 64");

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)

#endif // EVAL_MANAKA_MANAKA_ACCUMULATOR_H_INCLUDED
