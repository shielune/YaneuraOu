// Manaka の f32 評価を、局面の一覧に対して TSV で書き出す。Rust の dump_eval example と同じ列と書式。
//
//   eval_dump --weights X.mnw --sfens F.sfen [--limit N] [--copy-pair]
//
// 入力は 1 行 1 局面。先頭の "sfen " は捨てる。空行と # で始まる行は読み飛ばす。読めない行があれば、
// 出力は局面の番号で突き合わせるので、行をずらさないために全体を止める。
//
// 出力(タブ区切り、見出しなし、1 局面 1 行)
//   index        読んだ行の通し番号(0 から)
//   sfen         読んだ行(先頭の "sfen " を除く)
//   value_stm    手番側から見た値(tanh の後)
//   pre_stm      同じく tanh の前
//   value_black  先手から見た値(後手番なら符号を反転)
//   pre_black    同じく tanh の前
// f32 は Rust の {:.9e} と同じ形(仮数部 9 桁、指数は +も 0 埋めもなし)で書く。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../eval_f32.h"
#include "sfen_parse.h"

using namespace YaneuraOu::Eval::ManakaCore;

namespace {

const char* const kUsage = "usage: eval_dump --weights FILE.mnw --sfens FILE [--limit N] [--copy-pair]";

// Rust の {:.9e} と同じ書式。C の %.9e は指数が e+01 や e-05 になるので、整数に直す。
std::string format_f32(float v) {
	if (std::isnan(v))
		return "NaN";
	if (std::isinf(v))
		return v < 0 ? "-inf" : "inf";
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.9e", double(v));
	char* e = std::strchr(buf, 'e');
	const int exponent = std::atoi(e + 1);
	*e = '\0';
	char out[80];
	std::snprintf(out, sizeof(out), "%se%d", buf, exponent);
	return out;
}

std::string trim(const std::string& s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return s.substr(b, e - b);
}

} // namespace

int main(int argc, char** argv) {
	std::string weights_path, sfen_path;
	size_t limit = SIZE_MAX;
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
		else if (flag == "--copy-pair")
			opt.mmap_pair = false;
		else {
			std::fprintf(stderr, "unknown argument %s\n%s\n", flag.c_str(), kUsage);
			return 2;
		}
	}
	if (weights_path.empty() || sfen_path.empty()) {
		std::fprintf(stderr, "%s\n", kUsage);
		return 2;
	}

	std::string err;
	MnwWeights weights;
	if (!MnwWeights::load(weights_path, opt, &weights, &err)) {
		std::fprintf(stderr, "eval_dump: %s\n", err.c_str());
		return 1;
	}
	auto ev = EvaluatorF32::create(&weights, &err);
	if (!ev) {
		std::fprintf(stderr, "eval_dump: %s\n", err.c_str());
		return 1;
	}

	std::ifstream in(sfen_path);
	if (!in) {
		std::fprintf(stderr, "eval_dump: %s を開けない\n", sfen_path.c_str());
		return 1;
	}

	struct Row {
		std::string sfen;
		int stm;
		float value, pre;
	};
	std::vector<Row> rows;

	size_t line_no = 0;
	size_t finite = 0;
	double worst_tanh = 0.0;
	float min_pre = INFINITY, max_pre = -INFINITY, min_value = INFINITY, max_value = -INFINITY;
	std::string line;
	const auto started = std::chrono::steady_clock::now();
	while (rows.size() < limit && std::getline(in, line)) {
		++line_no;
		std::string t = trim(line);
		if (t.empty() || t[0] == '#')
			continue;
		if (t.compare(0, 5, "sfen ") == 0)
			t = trim(t.substr(5));
		BoardView board;
		if (!parse_sfen(t, &board, &err)) {
			std::fprintf(stderr, "%s:%zu: not a SFEN: %s (%s)\n", sfen_path.c_str(), line_no, t.c_str(), err.c_str());
			return 1;
		}
		EvaluatorF32::Result r;
		if (!ev->evaluate(board, &r)) {
			std::fprintf(stderr, "%s:%zu: 評価できない局面: %s\n", sfen_path.c_str(), line_no, t.c_str());
			return 1;
		}
		if (std::isfinite(r.value) && std::isfinite(r.pre)) {
			++finite;
			worst_tanh = std::fmax(worst_tanh, std::fabs(std::tanh(double(r.pre)) - double(r.value)));
		}
		min_pre = std::fmin(min_pre, r.pre);
		max_pre = std::fmax(max_pre, r.pre);
		min_value = std::fmin(min_value, r.value);
		max_value = std::fmax(max_value, r.value);
		rows.push_back({ t, board.side_to_move, r.value, r.pre });
	}
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

	std::string text;
	text.reserve(rows.size() * 200);
	for (size_t i = 0; i < rows.size(); ++i) {
		const Row& r = rows[i];
		const float sign = r.stm == 0 ? 1.0f : -1.0f;
		text += std::to_string(i);
		text += '\t';
		text += r.sfen;
		text += '\t';
		text += format_f32(r.value);
		text += '\t';
		text += format_f32(r.pre);
		text += '\t';
		text += format_f32(r.value * sign);
		text += '\t';
		text += format_f32(r.pre * sign);
		text += '\n';
	}
	std::fwrite(text.data(), 1, text.size(), stdout);
	std::fflush(stdout);

	const size_t n = rows.size();
	std::fprintf(stderr, "shape %s, pair %s\n", mnw_shape_name(weights.header()).c_str(), weights.pair_is_mapped() ? "mmap" : "copied");
	std::fprintf(stderr, "positions %zu, finite value and pre %zu\n", n, finite);
	std::fprintf(stderr, "evaluated in %.3f s (%.1f us/position, parse + evaluate)\n", elapsed, elapsed / double(n ? n : 1) * 1e6);
	std::fprintf(stderr, "value range %.6f .. %.6f, pre range %.6f .. %.6f\n", double(min_value), double(max_value), double(min_pre), double(max_pre));
	std::fprintf(stderr, "max |tanh(pre) - value| over the run %.3e (tanh taken in f64)\n", worst_tanh);
	return 0;
}
