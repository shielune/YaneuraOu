#if defined(EVAL_LEARN)
// HalfKPL (HalfKP cross-product linear) 用の feature dump コマンド。
//
// 入力: binpack (raw PackedSfenValue ×N、40B/local)
// 出力: <prefix>.idx.bin  (int32 [N, 76] row-major、HalfKPL active indices)
//                          先頭 38 列が Friend 側 (+ で寄与)、後半 38 列が Enemy 側 (- で寄与)
//       <prefix>.y.bin    (float32 [N]) 教師の評価値 (centipawns、BLACK POV 変換済)
//       <prefix>.meta.txt
//
// Ridge fit 側では Enemy 側に係数 -1 を立てて X を (N, 76) sparse one-hot 行列扱いに
// すれば、Friend - Enemy の重み共有が自然に表現できる。

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

constexpr int HKPL_ACTIVE_HALF = 38; // 38 駒 × 2 side = 76 列
constexpr int HKPL_ACTIVE      = HKPL_ACTIVE_HALF * 2;

void halfkpl_dump_cmd(Position& /*pos*/, std::istringstream& is) {
	std::string input_file;
	std::string output_prefix = "halfkpl_features";
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
		else std::cerr << "halfkpl_dump: unknown token: " << token << std::endl;
	}

	if (input_file.empty()) {
		std::cerr << "halfkpl_dump: input is required" << std::endl;
		return;
	}

	const size_t REC_SIZE = sizeof(Learner::PackedSfenValue);
	if (REC_SIZE != 40) {
		std::cerr << "halfkpl_dump: unexpected PackedSfenValue size " << REC_SIZE << std::endl;
		return;
	}

	std::ifstream ifs(input_file, std::ios::binary);
	if (!ifs) {
		std::cerr << "halfkpl_dump: failed to open " << input_file << std::endl;
		return;
	}
	ifs.seekg(0, std::ios::end);
	const uint64_t file_size = (uint64_t)ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	const uint64_t total_records = file_size / REC_SIZE;
	if (skip >= total_records) {
		std::cerr << "halfkpl_dump: skip >= total_records (" << total_records << ")" << std::endl;
		return;
	}
	uint64_t n = std::min(max_positions, total_records - skip);

	std::cout << "halfkpl_dump:" << std::endl
	          << "  input          = " << input_file << std::endl
	          << "  output_prefix  = " << output_prefix << std::endl
	          << "  feat_dim       = " << NUM_HKPL_FEATURES << std::endl
	          << "  active         = " << HKPL_ACTIVE << " (friend "
	          << HKPL_ACTIVE_HALF << " + enemy " << HKPL_ACTIVE_HALF << ")" << std::endl
	          << "  total_records  = " << total_records << std::endl
	          << "  skip           = " << skip << std::endl
	          << "  N to extract   = " << n << std::endl;

	if (skip > 0)
		ifs.seekg((std::streamoff)(skip * REC_SIZE), std::ios::beg);

	std::vector<Learner::PackedSfenValue> psvs(n);
	ifs.read(reinterpret_cast<char*>(psvs.data()), (std::streamsize)(n * REC_SIZE));
	if ((uint64_t)ifs.gcount() != n * REC_SIZE) {
		std::cerr << "halfkpl_dump: short read (" << ifs.gcount() << " bytes)" << std::endl;
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
		std::cerr << "halfkpl_dump: WARN Threads option (" << existing_threads
		          << ") < requested (" << max_threads << "). Clamping." << std::endl;
		max_threads = existing_threads;
	}
	std::cout << "  threads        = " << max_threads << std::endl;

	std::vector<int32_t> X((size_t)n * HKPL_ACTIVE);
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
			int f_idx[HKPL_ACTIVE_HALF], e_idx[HKPL_ACTIVE_HALF];
			extract_halfkpl_indices(pos, f_idx, e_idx);
			int32_t* row = X.data() + (size_t)i * HKPL_ACTIVE;
			for (int k = 0; k < HKPL_ACTIVE_HALF; ++k) row[k] = f_idx[k];
			for (int k = 0; k < HKPL_ACTIVE_HALF; ++k) row[HKPL_ACTIVE_HALF + k] = e_idx[k];
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
			std::memcpy(X.data() + kept * HKPL_ACTIVE, X.data() + i * HKPL_ACTIVE,
			            sizeof(int32_t) * HKPL_ACTIVE);
			y[kept] = y[i];
		}
		++kept;
	}
	if (kept != n)
		std::cout << "halfkpl_dump: dropped " << (n - kept) << " invalid records" << std::endl;

	const std::string xpath = output_prefix + ".idx.bin";
	const std::string ypath = output_prefix + ".y.bin";
	const std::string mpath = output_prefix + ".meta.txt";

	{
		std::ofstream ofs(xpath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(X.data()),
		          (std::streamsize)(sizeof(int32_t) * kept * HKPL_ACTIVE));
	}
	{
		std::ofstream ofs(ypath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(y.data()),
		          (std::streamsize)(sizeof(float) * kept));
	}
	{
		std::ofstream ofs(mpath);
		ofs << "N=" << kept << "\n"
		    << "D=" << NUM_HKPL_FEATURES << "\n"
		    << "active=" << HKPL_ACTIVE << "\n"
		    << "active_half=" << HKPL_ACTIVE_HALF << "\n"
		    << "fe_end=" << NUM_HKPL_FE_END << "\n"
		    << "king_sq=" << NUM_HKPL_KING_SQ << "\n"
		    << "layout=row major, first half = friend(+), second half = enemy(-)\n"
		    << "dtype=int32 (X) / float32 (y)\n";
	}

	std::cout << "halfkpl_dump: wrote " << kept << " rows to " << xpath << " / " << ypath << std::endl;
}

} // namespace HumanLike
} // namespace Eval
#endif // EVAL_LEARN
