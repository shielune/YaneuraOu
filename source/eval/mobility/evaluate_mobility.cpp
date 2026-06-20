#include "../../config.h"

#if defined(EVAL_MOBILITY)

#include <iostream>

#include "../../types.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../humanlike/humanlike_eval.h"

// MB 評価関数 (Mobility, Ridge-fit 線形モデル)。
//
// MOBILITY_VARIANT == 1 (デフォルト):
//   重みは mobility_weights_embedded.cpp にコンパイル時焼き込み。外部ファイル不要。
//
// MOBILITY_VARIANT == 2/3/4:
//   パラメータ数が増大するため embedded には持たない。
//   起動時に EvalDir/eval.bin を load_mobility_weights() で読み込む。

#include <string>
#include "../../usi_option.h"

#if !defined(MOBILITY_VARIANT) || MOBILITY_VARIANT == 1

namespace Eval { namespace Mobility {

extern const float kEmbeddedMobilityWeights[HumanLike::NUM_FEATURES];
extern const int   kEmbeddedMobilityWeightsSize;

} } // namespace Eval::Mobility

#endif // MOBILITY_VARIANT == 1

namespace Eval {

void init() {}

void load_eval() {
#if !defined(MOBILITY_VARIANT) || MOBILITY_VARIANT == 1
	std::cerr << "info string EVAL_MOBILITY v1: using embedded weights ("
	          << Mobility::kEmbeddedMobilityWeightsSize << " params)" << std::endl;
#else
	// バリアント2〜4: EvalDir/eval.bin を外部ファイルとして読み込む
	const std::string eval_dir = Options["EvalDir"];
	const std::string path = eval_dir + "/eval.bin";
	if (!HumanLike::load_mobility_weights(path))
		std::cerr << "info string EVAL_MOBILITY v" << MOBILITY_VARIANT
		          << ": WARNING failed to load " << path << std::endl;
#endif
}

void print_eval_stat(Position& /*pos*/) {
	std::cout << "--- EVAL STAT: EVAL_MOBILITY variant="
#if defined(MOBILITY_VARIANT)
	          << MOBILITY_VARIANT
#else
	          << 1
#endif
	          << " params=" << HumanLike::NUM_FEATURES << std::endl;
}

void evaluate_with_no_return(const Position& /*pos*/) {}

Value compute_eval(const Position& pos) {
	float feat[HumanLike::NUM_FEATURES];
	HumanLike::extract_features(pos, feat);
	double sum = 0.0;
#if !defined(MOBILITY_VARIANT) || MOBILITY_VARIANT == 1
	for (int i = 0; i < HumanLike::NUM_FEATURES; ++i)
		sum += (double)Mobility::kEmbeddedMobilityWeights[i] * (double)feat[i];
#else
	// load_mobility_weights() が g_mobility_weights[] に書き込んでいる。
	// humanlike_eval.cpp の mobility_score() と同じ経路を使う。
	const Value v = HumanLike::evaluate(pos, HumanLike::Mode::Mobility, false);
	return v;
#endif
	if (sum >  3000.0) sum =  3000.0;
	if (sum < -3000.0) sum = -3000.0;
	return Value(pos.side_to_move() == BLACK ? (int)sum : -(int)sum);
}

Value evaluate(const Position& pos) {
	return compute_eval(pos);
}

} // namespace Eval

#endif // EVAL_MOBILITY
