#include "../../config.h"

#if defined(EVAL_MANAKA)

#include <cmath>

#include "manaka_net.h"

// pre から内部値への写像。この翻訳単位は Makefile で -ffast-math を外し、-ffp-contract=off でビルドする。
// -ffast-math (-ffinite-math-only) の下では std::isfinite が定数 true に畳まれうるので、有限性の判定と
// float から int への変換 (NaN や範囲外は未定義) はこのファイルに置く。呼び出し側は noinline の境界の
// 向こうの結果 (bool) だけを見る。

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

OutputMap::OutputMap() : OutputMap(1512.173, 0.9, 0.9999) {}

OutputMap::OutputMap(double coef, double unit, double value_clamp) :
	scale_(coef * unit), limit_(std::atanh(value_clamp)) {}

__attribute__((noinline)) bool OutputMap::to_internal(float pre, int* out) const {
	if (!std::isfinite(pre))
		return false;
	double x = static_cast<double>(pre);
	if (x > limit_)
		x = limit_;
	else if (x < -limit_)
		x = -limit_;
	*out = static_cast<int>(x * scale_);
	return true;
}

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)
