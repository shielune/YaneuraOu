#ifndef _HCE_MLP_H_INCLUDED_
#define _HCE_MLP_H_INCLUDED_

// HCE v2 の全結合層 (入力 252、中間 32 を二段、出力 1、ReLU)。
//
//   h1 = relu(W1 x + b1)   W1 : [32][252]
//   h2 = relu(W2 h1 + b2)  W2 : [32][32]
//   y  = W3 h2 + b3        W3 : [32]
//
// 重みの受け取り方は二つ。
//
//  (1) 実行時に JSON を読む。USI option の HceMlpFile にパスを渡す。
//      key は次のいずれかを受ける (PyTorch の state_dict をそのまま dump したものを想定)。
//        W1 : "w1" / "fc1.weight" / "0.weight" / "net.0.weight" / "layers.0.weight"
//        b1 : "b1" / "fc1.bias"   / "0.bias"   / "net.0.bias"   / "layers.0.bias"
//        W2 : "w2" / "fc2.weight" / "2.weight" / "net.2.weight" / "layers.2.weight"
//        b2 : "b2" / "fc2.bias"   / "2.bias"   / "net.2.bias"   / "layers.2.bias"
//        W3 : "w3" / "fc3.weight" / "4.weight" / "net.4.weight" / "layers.4.weight"
//        b3 : "b3" / "fc3.bias"   / "4.bias"   / "net.4.bias"   / "layers.4.bias"
//      入れ子の [[...],[...]] でも平らな [...] でもよい。中の数値を順に読むだけ。
//      行列は row-major (出力 × 入力) で、PyTorch の Linear.weight と同じ並び。
//
//  (2) コンパイル時に焼き込む。同じ folder に hce_mlp_weights_embedded.h を置くと
//      __has_include で拾う。その header は
//          #define HCE_MLP_EMBEDDED 1
//          namespace YaneuraOu { namespace Eval { namespace Mobility {
//          inline constexpr float kHceMlpW1[] = { ... };  // 32*252
//          inline constexpr float kHceMlpB1[] = { ... };  // 32
//          inline constexpr float kHceMlpW2[] = { ... };  // 32*32
//          inline constexpr float kHceMlpB2[] = { ... };  // 32
//          inline constexpr float kHceMlpW3[] = { ... };  // 32
//          inline constexpr float kHceMlpB3    = ...;
//          } } }
//      という形で書くこと (source/Makefile は触れないので、header 内で定義まで済ませる)。
//
// どちらも無いときは仮の重みで動く。値に意味は無いが、三層と ReLU が実際に通ることは
// 確かめられる。仮の重みを使っている間は load 時に info string で言う。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../humanlike/humanlike_eval.h"

namespace YaneuraOu {
namespace Eval {
namespace Mobility {

// 全結合層は plain の並びだけを入力に取る。幅を変えたければ焼き直しになる。
constexpr int HCE_MLP_IN = HumanLike::NUM_MATERIAL_FOLDED
                         + HumanLike::NUM_BOARD_FEATURES_FOLDED + HumanLike::NUM_HAND_FEATURES
                         + HumanLike::NUM_KING_FEATURES; // 231
constexpr int HCE_MLP_H1 = 32;
constexpr int HCE_MLP_H2 = 32;

struct HceMlp {
	float w1[HCE_MLP_H1 * HCE_MLP_IN];
	float b1[HCE_MLP_H1];
	float w2[HCE_MLP_H2 * HCE_MLP_H1];
	float b2[HCE_MLP_H2];
	float w3[HCE_MLP_H2];
	float b3;

	// 仮の重み。決まった種から作るので毎回同じ。中間層が全部 0 で潰れないように
	// 正負を混ぜてある。
	void fill_placeholder() {
		uint32_t s = 20260914u;
		auto next = [&s](float scale) {
			s = s * 1664525u + 1013904223u;
			return ((float)((s >> 8) & 0xFFFF) / 32768.0f - 1.0f) * scale;
		};
		// 出す値が内部単位で数百の桁に来るように倍率を決めてある。
		// 小さすぎると (int) で全部 0 になり、経路が生きていることが見えない。
		for (float& v : w1) v = next(0.05f);
		for (float& v : b1) v = 0.0f;
		for (float& v : w2) v = next(0.5f);
		for (float& v : b2) v = 0.0f;
		for (float& v : w3) v = next(50.0f);
		b3 = 0.0f;
	}

	float forward(const float* x) const {
		float h1[HCE_MLP_H1];
		for (int i = 0; i < HCE_MLP_H1; ++i) {
			float a = b1[i];
			const float* row = w1 + (size_t)i * HCE_MLP_IN;
			for (int j = 0; j < HCE_MLP_IN; ++j) a += row[j] * x[j];
			h1[i] = a > 0.0f ? a : 0.0f;
		}
		float h2[HCE_MLP_H2];
		for (int i = 0; i < HCE_MLP_H2; ++i) {
			float a = b2[i];
			const float* row = w2 + (size_t)i * HCE_MLP_H1;
			for (int j = 0; j < HCE_MLP_H1; ++j) a += row[j] * h1[j];
			h2[i] = a > 0.0f ? a : 0.0f;
		}
		float y = b3;
		for (int i = 0; i < HCE_MLP_H2; ++i) y += w3[i] * h2[i];
		return y;
	}
};

// JSON から HceMlp を読む。失敗したら false を返して err に理由を入れる。
bool load_hce_mlp_json(const std::string& path, HceMlp& out, std::string& err);

} // namespace Mobility
} // namespace Eval
} // namespace YaneuraOu

#endif
