#include "../../config.h"

#if defined(EVAL_MANAKA)

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../../types.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../../misc.h"
#include "../../usioption.h"
#include "evaluate_manaka.h"

// Manaka 評価関数の骨組み。評価値は常に 0 で、nn.bin はまだ読まない。
//
// 骨組みは YaneuraOu に配線するための殻で、本実装 (nn.bin の読み込み、トークン集合の
// 差分更新、W1 以降の演算) は後から source/eval/manaka/ の中だけに入る。
// 共有ファイル (config.h, Makefile, position.*, yaneuraou-search.cpp, normal_test_cmd.cpp,
// evaluate_nnue.cpp) は骨組みと一緒に一度だけ変えてあり、本実装は触らなくてよい。
// 約束事は eval/manaka/API.md を見ること。

namespace YaneuraOu {
namespace Eval {

namespace {

// EvalDir と EvalFile の現在値。既定は NNUE と同じ eval と nn.bin。
std::string g_eval_dir  = "eval";
std::string g_eval_file = "nn.bin";

// load_eval() が読み込み済みか。EvalDir と EvalFile が変わったら倒して、
// 次の isready で読み直させる (NNUE の eval_loaded と同じ作法)。
bool g_loaded = false;

} // namespace

void add_options(OptionsMap& options, ThreadPool&) {

	// 評価関数フォルダ。相対なら実行ファイルのあるフォルダ基準で解決する (NNUE と同じ)。
	options.add("EvalDir", Option(g_eval_dir.c_str(), [](const Option& o) {
		const std::string dir = (std::string)o;
		if (dir != g_eval_dir)
		{
			g_eval_dir = dir;
			g_loaded   = false;
		}
		return std::nullopt;
	}));

	// 評価関数ファイル名。NNUE は WASM の時だけ持つが、Manaka は常に持つ。
	options.add("EvalFile", Option(g_eval_file.c_str(), [](const Option& o) {
		const std::string file = (std::string)o;
		if (file != g_eval_file)
		{
			g_eval_file = file;
			g_loaded    = false;
		}
		return std::nullopt;
	}));
}

// "isready" のたびに呼ばれる。骨組みではファイルの有無を言うだけで、中身は読まない。
void load_eval() {
	if (g_loaded)
		return;

	const std::string path = Path::Combine(Path::Combine(Directory::GetBinaryFolder(), g_eval_dir), g_eval_file);
	sync_cout << "info string loading eval file : " << path << sync_endl;

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open())
	{
		sync_cout << "info string Manaka: eval file not found: " << path
		          << " (skeleton: the evaluation is always 0)" << sync_endl;
	}
	else
	{
		stream.seekg(0, std::ios::end);
		const std::streamoff size = stream.tellg();
		sync_cout << "info string Manaka: found eval file (" << size << " bytes) but did not read it"
		          << " (skeleton: the evaluation is always 0)" << sync_endl;
	}

	g_loaded = true;
}

// 全計算。Position::set() と、差分更新できないときに呼ばれる。
// 骨組みでは常に 0 を返し、accumulator には触らない (旗は倒れたまま = 未計算)。
Value compute_eval(const Position&) {
	return VALUE_ZERO;
}

// 次の評価のために差分更新だけ進める入口。骨組みでは何もしない。
void evaluate_with_no_return(const Position&) {
}

// 評価値の内訳表示 (デバッグ用)。
void print_eval_stat(Position&) {
	std::cout << "--- EVAL STAT: EVAL_MANAKA (skeleton)" << std::endl
	          << "  eval = 0 (always)" << std::endl;
}

// 評価関数本体。手番側から見た値を返す。骨組みでは常に 0。
Value evaluate(const Position&) {
	return VALUE_ZERO;
}

namespace Manaka {

void trace(const Position&) {
	sync_cout << "manaka pre 0 internal 0" << sync_endl;
}

void test_walk(IEngine&, std::istringstream&) {
	sync_cout << "test manaka_walk: not implemented (skeleton)" << sync_endl;
}

void test_bench(IEngine&, std::istringstream&) {
	sync_cout << "test manaka_bench: not implemented (skeleton)" << sync_endl;
}

} // namespace Manaka

} // namespace Eval
} // namespace YaneuraOu

#endif // EVAL_MANAKA
