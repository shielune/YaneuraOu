#include "hce_net.h"

#if defined(USE_HCE_EXPLAIN)

#include <algorithm>

namespace YaneuraOu {
namespace Eval {
namespace Hce {

bool Mlp::assign(const std::vector<float>& v, int in, int hidden) {
	if (in <= 0 || hidden <= 0 || in > (1 << 16) || hidden > 4096)
		return false;
	const size_t n = size_t(hidden) * in + hidden + size_t(hidden) * hidden + hidden + hidden + 1;
	if (v.size() != n)
		return false;
	size_t at = 0;
	auto take = [&](std::vector<float>& out, size_t count) {
		out.assign(v.begin() + at, v.begin() + at + count);
		at += count;
	};
	take(w1_, size_t(hidden) * in);
	take(b1_, hidden);
	take(w2_, size_t(hidden) * hidden);
	take(b2_, hidden);
	take(w3_, hidden);
	b3_     = v[at];
	in_     = in;
	hidden_ = hidden;
	return true;
}

float Mlp::forward(const float* x) const {
	std::vector<float> a(hidden_), b(hidden_);
	for (int h = 0; h < hidden_; ++h) {
		const float* row = &w1_[size_t(h) * in_];
		float sum = b1_[h];
		for (int i = 0; i < in_; ++i) sum += row[i] * x[i];
		a[h] = std::max(sum, 0.0f);
	}
	for (int h = 0; h < hidden_; ++h) {
		const float* row = &w2_[size_t(h) * hidden_];
		float sum = b2_[h];
		for (int i = 0; i < hidden_; ++i) sum += row[i] * a[i];
		b[h] = std::max(sum, 0.0f);
	}
	float out = b3_;
	for (int h = 0; h < hidden_; ++h) out += w3_[h] * b[h];
	return out;
}

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
