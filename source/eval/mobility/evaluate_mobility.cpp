#include "../../config.h"

#if defined(EVAL_MOBILITY)

#include <iostream>

#include "../../types.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../humanlike/humanlike_eval.h"

// MB 評価関数 (Mobility, 164 params Ridge-fit) を embedded weights で動かす
// 純粋 standalone eval。NNUE と異なり外部 nn.bin を要求しない。
//
// 重みは scripts/humanlike_eval/embed_mobility_weights.py で
// mobility_weights_embedded.cpp に焼き込まれる。

namespace Eval { namespace Mobility {

extern const float kEmbeddedMobilityWeights[164];
extern const int   kEmbeddedMobilityWeightsSize;

} } // namespace Eval::Mobility

namespace Eval {

void init() {}

void load_eval() {
	// 重みはコンパイル時に焼き込み済み。読み込み処理は不要。
	std::cerr << "info string EVAL_MOBILITY: using embedded mobility weights ("
	          << Mobility::kEmbeddedMobilityWeightsSize << " params)" << std::endl;
}

void print_eval_stat(Position& /*pos*/) {
	std::cout << "--- EVAL STAT: EVAL_MOBILITY (no breakdown available)" << std::endl;
}

void evaluate_with_no_return(const Position& /*pos*/) {
	// 差分計算は持たない。何もしない。
}

Value compute_eval(const Position& pos) {
	float feat[HumanLike::NUM_FEATURES];
	HumanLike::extract_features(pos, feat);
	double sum = 0.0;
	for (int i = 0; i < HumanLike::NUM_FEATURES; ++i)
		sum += (double)Mobility::kEmbeddedMobilityWeights[i] * (double)feat[i];
	if (sum >  3000.0) sum =  3000.0;
	if (sum < -3000.0) sum = -3000.0;
	int score = (int)sum;
	return Value(pos.side_to_move() == BLACK ? score : -score);
}

Value evaluate(const Position& pos) {
	return compute_eval(pos);
}

} // namespace Eval

#endif // EVAL_MOBILITY
