// Manaka の f32 評価の、局面あたりの全計算の時間を測る。
//
//   taskset -c 2 eval_bench --weights X.mnw --sfens F.sfen [--limit N] [--passes P] [--warmup W]
//                           [--copy-pair] [--stages]
//
// 局面ごとに「盤から特徴を作って W1 以降まで走らせる」全計算を 1 回ずつ、rdtsc で挟んで測る。
// 局面の読み取り(SFEN の解析)は測らない。warmup 回の通しは捨てる(pair 表のページを引き込むため)。
// 残りの passes 回の通しのすべての測定から、平均(= 合計 / 回数)、中央値、最小、最大、99 パーセンタイルを出す。
// --stages を付けると、続けて段ごとの時間(平均、マイクロ秒)も出す(計時の分だけ全体より少し遅い)。
// 表示の「記録」と比べるときは平均を使う(Rust の dump_eval は合計 / 局面数を出す)。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "../eval_f32.h"
#include "sfen_parse.h"

using namespace YaneuraOu::Eval::ManakaCore;

namespace {

const char* const kUsage = "usage: eval_bench --weights FILE.mnw --sfens FILE [--limit N] [--passes P] [--warmup W] [--copy-pair] [--stages]";

const char* const kStageNames[kNumEvalStages] = { "tokens", "powers", "newton+rms", "pair", "W1", "W2", "value+tanh" };

inline uint64_t tsc_begin() {
	_mm_lfence();
	const uint64_t t = __rdtsc();
	_mm_lfence();
	return t;
}

inline uint64_t tsc_end() {
	unsigned aux;
	const uint64_t t = __rdtscp(&aux);
	_mm_lfence();
	return t;
}

// 1 秒あたりの TSC の刻み。steady_clock と並べて 200 ms 測る。
double calibrate_tsc_hz() {
	const auto t0 = std::chrono::steady_clock::now();
	const uint64_t c0 = tsc_begin();
	while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.2) {
	}
	const uint64_t c1 = tsc_end();
	const auto t1 = std::chrono::steady_clock::now();
	return double(c1 - c0) / std::chrono::duration<double>(t1 - t0).count();
}

std::string trim(const std::string& s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return s.substr(b, e - b);
}

double percentile(const std::vector<double>& sorted, double q) {
	if (sorted.empty())
		return 0.0;
	const double pos = q * double(sorted.size() - 1);
	const size_t lo = size_t(pos);
	const size_t hi = std::min(lo + 1, sorted.size() - 1);
	return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - double(lo));
}

} // namespace

int main(int argc, char** argv) {
	std::string weights_path, sfen_path;
	size_t limit = SIZE_MAX;
	int passes = 6, warmup = 2;
	bool stages = false;
	MnwLoadOptions opt;

	for (int i = 1; i < argc; ++i) {
		const std::string flag = argv[i];
		auto value = [&](const char* name) -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value\n%s\n", name, kUsage);
				std::exit(2);
			}
			return argv[++i];
		};
		if (flag == "--weights")
			weights_path = value("--weights");
		else if (flag == "--sfens")
			sfen_path = value("--sfens");
		else if (flag == "--limit")
			limit = std::strtoull(value("--limit"), nullptr, 10);
		else if (flag == "--passes")
			passes = std::atoi(value("--passes"));
		else if (flag == "--warmup")
			warmup = std::atoi(value("--warmup"));
		else if (flag == "--copy-pair")
			opt.mmap_pair = false;
		else if (flag == "--stages")
			stages = true;
		else {
			std::fprintf(stderr, "unknown argument %s\n%s\n", flag.c_str(), kUsage);
			return 2;
		}
	}
	if (weights_path.empty() || sfen_path.empty() || passes < 1 || warmup < 0) {
		std::fprintf(stderr, "%s\n", kUsage);
		return 2;
	}

	std::string err;
	MnwWeights weights;
	if (!MnwWeights::load(weights_path, opt, &weights, &err)) {
		std::fprintf(stderr, "eval_bench: %s\n", err.c_str());
		return 1;
	}
	auto ev = EvaluatorF32::create(&weights, &err);
	if (!ev) {
		std::fprintf(stderr, "eval_bench: %s\n", err.c_str());
		return 1;
	}

	std::vector<BoardView> boards;
	{
		std::ifstream in(sfen_path);
		if (!in) {
			std::fprintf(stderr, "eval_bench: %s を開けない\n", sfen_path.c_str());
			return 1;
		}
		std::string line;
		while (boards.size() < limit && std::getline(in, line)) {
			std::string t = trim(line);
			if (t.empty() || t[0] == '#')
				continue;
			if (t.compare(0, 5, "sfen ") == 0)
				t = trim(t.substr(5));
			BoardView b;
			if (!parse_sfen(t, &b, &err)) {
				std::fprintf(stderr, "eval_bench: not a SFEN: %s (%s)\n", t.c_str(), err.c_str());
				return 1;
			}
			boards.push_back(b);
		}
	}
	if (boards.empty()) {
		std::fprintf(stderr, "eval_bench: 局面が無い\n");
		return 1;
	}

	const double hz = calibrate_tsc_hz();
	std::printf("shape %s, pair %s, positions %zu, passes %d (warmup %d discarded), tsc %.1f MHz\n", mnw_shape_name(weights.header()).c_str(),
	            weights.pair_is_mapped() ? "mmap" : "copied", boards.size(), passes, warmup, hz / 1e6);

	double checksum = 0.0;
	std::vector<double> samples;
	samples.reserve(boards.size() * size_t(passes));
	for (int pass = 0; pass < warmup + passes; ++pass) {
		for (const BoardView& b : boards) {
			EvaluatorF32::Result r;
			const uint64_t t0 = tsc_begin();
			const bool ok = ev->evaluate(b, &r);
			const uint64_t t1 = tsc_end();
			if (!ok) {
				std::fprintf(stderr, "eval_bench: 評価できない局面がある\n");
				return 1;
			}
			checksum += double(r.pre);
			if (pass >= warmup)
				samples.push_back(double(t1 - t0) / hz * 1e6);
		}
	}

	double sum = 0.0;
	for (double s : samples)
		sum += s;
	std::vector<double> sorted = samples;
	std::sort(sorted.begin(), sorted.end());
	std::printf("full evaluation per position (us): mean %.2f  median %.2f  min %.2f  p99 %.2f  max %.2f  (samples %zu)\n", sum / double(samples.size()),
	            percentile(sorted, 0.5), sorted.front(), percentile(sorted, 0.99), sorted.back(), samples.size());

	if (stages) {
		EvalTrace total;
		total.clear();
		size_t count = 0;
		for (int pass = 0; pass < passes; ++pass) {
			for (const BoardView& b : boards) {
				EvaluatorF32::Result r;
				ev->evaluate(b, &r, &total);
				checksum += double(r.pre);
				++count;
			}
		}
		uint64_t all = 0;
		for (int s = 0; s < kNumEvalStages; ++s)
			all += total.ticks[s];
		std::printf("stages (us per position, traced passes):\n");
		for (int s = 0; s < kNumEvalStages; ++s) {
			const double us = double(total.ticks[s]) / hz * 1e6 / double(count);
			std::printf("  %-11s %8.2f  (%4.1f%%)\n", kStageNames[s], us, all ? 100.0 * double(total.ticks[s]) / double(all) : 0.0);
		}
		std::printf("  %-11s %8.2f\n", "total", double(all) / hz * 1e6 / double(count));
	}
	std::printf("checksum %.9g\n", checksum);
	return 0;
}
