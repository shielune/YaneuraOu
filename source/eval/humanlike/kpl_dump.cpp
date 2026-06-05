#if defined(EVAL_LEARN)
// KPL (K+P linear) 用の feature dump コマンド。
//
// 入力: binpack (raw PackedSfenValue ×N、40B/local)
// 出力: <prefix>.idx.bin  (int32 [N, 40] row-major、KPL active indices)
//       <prefix>.y.bin    (float32 [N]) 教師の評価値 (centipawns、BLACK POV 変換済)
//       <prefix>.meta.txt
//
// KPL は active 数が一定 (40 = 38 駒 + 2 玉) なので row-major 固定列でよい。
//
// 教師 binpack の psv.score は side-to-move POV、本実装の indices は BLACK POV
// (pieces_fb を使う) なので、WHITE 手番の局面では y の符号を反転する。

#include "humanlike_eval.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "../../position.h"
#include "../../thread.h"
#include "../../usi.h"
#include "../../learn/learn.h"

namespace Eval {
namespace HumanLike {

constexpr int KPL_ACTIVE = 40; // 38 P + 2 K

void kpl_dump_cmd(Position& /*pos*/, std::istringstream& is) {
	std::string input_file;
	std::string output_prefix = "kpl_features";
	uint64_t max_positions = 1000000;
	uint64_t skip = 0;
	int threads_req = -1;

	std::string token;
	while (is >> token) {
		if (token == "input")             is >> input_file;
		else if (token == "output_prefix")is >> output_prefix;
		else if (token == "max_positions")is >> max_positions;
		else if (token == "skip")         is >> skip;
		else if (token == "threads")      is >> threads_req;
		else std::cerr << "kpl_dump: unknown token: " << token << std::endl;
	}

	if (input_file.empty()) {
		std::cerr << "kpl_dump: input is required" << std::endl;
		return;
	}

	const size_t REC_SIZE = sizeof(Learner::PackedSfenValue);
	if (REC_SIZE != 40) {
		std::cerr << "kpl_dump: unexpected PackedSfenValue size " << REC_SIZE << std::endl;
		return;
	}

	std::ifstream ifs(input_file, std::ios::binary);
	if (!ifs) {
		std::cerr << "kpl_dump: failed to open " << input_file << std::endl;
		return;
	}
	ifs.seekg(0, std::ios::end);
	const uint64_t file_size = (uint64_t)ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	const uint64_t total_records = file_size / REC_SIZE;
	if (skip >= total_records) {
		std::cerr << "kpl_dump: skip >= total_records (" << total_records << ")" << std::endl;
		return;
	}
	uint64_t n = std::min(max_positions, total_records - skip);

	std::cout << "kpl_dump:" << std::endl
	          << "  input          = " << input_file << std::endl
	          << "  output_prefix  = " << output_prefix << std::endl
	          << "  feat_dim       = " << NUM_KPL_FEATURES << std::endl
	          << "  active         = " << KPL_ACTIVE << std::endl
	          << "  total_records  = " << total_records << std::endl
	          << "  skip           = " << skip << std::endl
	          << "  N to extract   = " << n << std::endl;

	if (skip > 0)
		ifs.seekg((std::streamoff)(skip * REC_SIZE), std::ios::beg);

	std::vector<Learner::PackedSfenValue> psvs(n);
	ifs.read(reinterpret_cast<char*>(psvs.data()), (std::streamsize)(n * REC_SIZE));
	if ((uint64_t)ifs.gcount() != n * REC_SIZE) {
		std::cerr << "kpl_dump: short read (" << ifs.gcount() << " bytes)" << std::endl;
		return;
	}
	ifs.close();

#ifdef _OPENMP
	int max_threads = (threads_req > 0) ? threads_req : omp_get_max_threads();
#else
	int max_threads = 1;
#endif

	int existing_threads = (int)Threads.size();
	if (existing_threads < max_threads) {
		std::cerr << "kpl_dump: WARN Threads option (" << existing_threads
		          << ") < requested (" << max_threads << "). Clamping." << std::endl;
		max_threads = existing_threads;
	}
	std::cout << "  threads        = " << max_threads << std::endl;

	std::vector<int32_t> X((size_t)n * KPL_ACTIVE);
	std::vector<float>   y(n);
	std::vector<uint8_t> valid(n, 0);

#ifdef _OPENMP
	#pragma omp parallel num_threads(max_threads)
#endif
	{
#ifdef _OPENMP
		int tid = omp_get_thread_num();
#else
		int tid = 0;
#endif
		auto th = Threads[tid];
		Position& pos = th->rootPos;
		StateInfo si;

#ifdef _OPENMP
		#pragma omp for schedule(dynamic, 1024)
#endif
		for (int64_t i = 0; i < (int64_t)n; ++i) {
			const auto& psv = psvs[(size_t)i];
			if (pos.set_from_packed_sfen(psv.sfen, &si, th).is_not_ok())
				continue;
			int idx[KPL_ACTIVE];
			int got = extract_kpl_indices(pos, idx);
			if (got != KPL_ACTIVE) continue;
			std::memcpy(X.data() + (size_t)i * KPL_ACTIVE, idx, sizeof(int32_t) * KPL_ACTIVE);
			float ys = (float)psv.score;
			if (pos.side_to_move() == WHITE) ys = -ys;
			y[(size_t)i] = ys;
			valid[(size_t)i] = 1;
		}
	}

	uint64_t kept = 0;
	for (uint64_t i = 0; i < n; ++i) {
		if (!valid[i]) continue;
		if (kept != i) {
			std::memcpy(X.data() + kept * KPL_ACTIVE, X.data() + i * KPL_ACTIVE,
			            sizeof(int32_t) * KPL_ACTIVE);
			y[kept] = y[i];
		}
		++kept;
	}
	if (kept != n)
		std::cout << "kpl_dump: dropped " << (n - kept) << " invalid records" << std::endl;

	const std::string xpath = output_prefix + ".idx.bin";
	const std::string ypath = output_prefix + ".y.bin";
	const std::string mpath = output_prefix + ".meta.txt";

	{
		std::ofstream ofs(xpath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(X.data()),
		          (std::streamsize)(sizeof(int32_t) * kept * KPL_ACTIVE));
	}
	{
		std::ofstream ofs(ypath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(y.data()),
		          (std::streamsize)(sizeof(float) * kept));
	}
	{
		std::ofstream ofs(mpath);
		ofs << "N=" << kept << "\n"
		    << "D=" << NUM_KPL_FEATURES << "\n"
		    << "active=" << KPL_ACTIVE << "\n"
		    << "P_features=" << NUM_KPL_P_FEATURES << "\n"
		    << "K_features=" << NUM_KPL_K_FEATURES << "\n"
		    << "dtype=int32 (X) / float32 (y)\n"
		    << "X_shape=(N,active) row-major\n";
	}

	std::cout << "kpl_dump: wrote " << kept << " rows to " << xpath << " / " << ypath << std::endl;
}

} // namespace HumanLike
} // namespace Eval
#endif // EVAL_LEARN
