#ifndef EVAL_MANAKA_MANAKA_ACCUMULATOR_H_INCLUDED
#define EVAL_MANAKA_MANAKA_ACCUMULATOR_H_INCLUDED

// Manaka 評価の差分計算用の枠。StateInfo の「do_move でコピーされない」領域に置く。
// position.h が include するので、YaneuraOu の型は types.h の Value しか使わない。
// 約束事 (不変条件、フラグの扱い) は eval/manaka/API.md に書いてある。
//
// 骨組みでは中身は空で、旗 (computed_accumulation, computed_score) だけが意味を持つ。
// 配列の中身を埋めるのは本実装 (source/eval/manaka/ の中だけ)。

#include <cstdint>

#include "../../config.h"
#include "../../types.h"

#if defined(EVAL_MANAKA)

// 形の上限。ビルド時の定数で、これを超える形のファイルは読み込みで拒否する。
// 既定は dim128、order3、pair16 (設計書の base 形)。幅広版 (256、4、32) は
// -DMANAKA_MAX_DIM=256 -DMANAKA_MAX_ORDER=4 -DMANAKA_MAX_PAIR_DIM=32 で作る。
#if !defined(MANAKA_MAX_DIM)
#define MANAKA_MAX_DIM 128
#endif
#if !defined(MANAKA_MAX_ORDER)
#define MANAKA_MAX_ORDER 3
#endif
#if !defined(MANAKA_MAX_PAIR_DIM)
#define MANAKA_MAX_PAIR_DIM 16
#endif

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

constexpr int kMaxDim     = MANAKA_MAX_DIM;      // 埋め込みの幅 (チャネル数)
constexpr int kMaxOrder   = MANAKA_MAX_ORDER;    // power sum の最大次数
constexpr int kMaxPairDim = MANAKA_MAX_PAIR_DIM; // pair の幅
constexpr int kFrames     = 2;                   // 先手枠と後手枠
constexpr int kMaxTokens  = 40;                  // 1 枠あたりのトークン数の上限 (盤上の駒と持ち駒の枚数の和)

// StateInfo が memcpy と memset で扱われる値型なので、可変長の所有物 (vector など) は持てない。
// AVX 系の整列読み書きのために 64 バイト境界に置く (NNUE の Accumulator と同じ)。
struct alignas(64) Accumulator {
	// 枠ごとの power sum。整数で厳密に持つ (差分更新の結果が全計算とビット単位で一致する)。
	// p[枠][k-1][チャネル] = Σ_{トークン t} embed[t][チャネル]^k
	std::int64_t  p[kFrames][kMaxOrder][kMaxDim];

	// 枠ごとの pair の和。整数で厳密に持つ。
	// pair[枠][j] = Σ_{対 {a,b}} pair_table[pair_index(a,b)][j]
	std::int32_t  pair[kFrames][kMaxPairDim];

	// 枠ごとのトークン id の一覧 (先頭から ntok 個が有効)。id は 0 から 2343 なので u16 に収まる。
	// 差分更新はこの一覧と現局面の一覧の差 (R と A) から行う。
	std::uint16_t tok[kFrames][kMaxTokens];

	// 有効なトークン数。先手枠と後手枠で同じ (同じ駒の集合を別の枠から読むだけなので)。
	std::uint8_t  ntok = 0;

	// p、pair、tok、ntok がそろって有効か。そろってから最後に立てる。
	bool          computed_accumulation = false;

	// score が現局面の値か。手番に依存するので do_null_move() でも倒す。
	bool          computed_score = false;

	// 手番側から見た内部値。computed_score が真のときだけ意味を持つ。
	Value         score = VALUE_ZERO;
};

static_assert(alignof(Accumulator) == 64, "Manaka::Accumulator must be 64-byte aligned");
static_assert(sizeof(Accumulator) % 64 == 0, "Manaka::Accumulator size must be a multiple of 64");

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)

#endif // EVAL_MANAKA_MANAKA_ACCUMULATOR_H_INCLUDED
