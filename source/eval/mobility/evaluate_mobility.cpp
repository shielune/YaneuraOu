#include "../../config.h"

#if defined(EVAL_MOBILITY)

#include <iostream>

#include "../../types.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../humanlike/humanlike_eval.h"

// MB 評価関数 (Mobility, Ridge-fit 線形モデル)。
//
// 起動時に EvalDir/weights.bin (MOBI 形式) を load_mobility_weights() で読み込む。
// ファイルが存在しない場合はエラーを出力して終了する。
// MobilityWeightsFile USI オプションで上書きも可能。

#include <string>
#include "../../usi_option.h"
#include "../../usi.h"

namespace Eval {

void init() {}

void load_eval() {
	const std::string eval_dir = Options["EvalDir"];
	const std::string path = eval_dir + "/weights.bin";
	if (!HumanLike::load_mobility_weights(path)) {
		sync_cout << "Error! : failed to read " << path << sync_endl;
		std::exit(1);
	}
}

void print_eval_stat(Position& /*pos*/) {
	std::cout << "--- EVAL STAT: EVAL_MOBILITY"
	          << " params=" << HumanLike::NUM_FEATURES << std::endl;
}

void evaluate_with_no_return(const Position& /*pos*/) {}

Value compute_eval(const Position& pos) {
	const Value v = HumanLike::evaluate(pos, HumanLike::Mode::Mobility, false);
	return v;
}

Value evaluate(const Position& pos) {
	return compute_eval(pos);
}

} // namespace Eval

#endif // EVAL_MOBILITY
