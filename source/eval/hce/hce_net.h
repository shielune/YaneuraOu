#ifndef HCE_NET_H_INCLUDED
#define HCE_NET_H_INCLUDED

#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include <vector>

namespace YaneuraOu {
namespace Eval {
namespace Hce {

// 入力 → 隠れ層 → 隠れ層 → 1 の小さな全結合ネットワーク。層の間は ReLU。
// 値の並びは scripts/export_hce_explain.py の節と同じ。
//   W1 (隠れ層 x 入力、1 行が 1 ユニット)、b1、W2 (隠れ層 x 隠れ層)、b2、W3 (隠れ層)、b3 (1)
class Mlp {
public:
	// 値の数が幅と合わなければ false。
	bool assign(const std::vector<float>& values, int in, int hidden);
	bool valid() const { return hidden_ != 0; }
	int  in() const { return in_; }
	int  hidden() const { return hidden_; }

	// 出力は 1 つ。x は in() 個。
	float forward(const float* x) const;

private:
	int in_ = 0, hidden_ = 0;
	std::vector<float> w1_, b1_, w2_, b2_, w3_;
	float b3_ = 0.0f;
};

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
#endif // HCE_NET_H_INCLUDED
