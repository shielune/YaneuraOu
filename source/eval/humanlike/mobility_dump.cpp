#if defined(EVAL_LEARN)
// MB (Mobility learned) 用の feature dump コマンド。
//
// 入力: binpack (raw PackedSfenValue ×N、40B/local)
// 出力: <prefix>.X.bin  (float32 [N, 164] row-major), 利き feature
//       <prefix>.y.bin  (float32 [N]) 教師の評価値 (centipawns、BLACK POV 変換済)
//       <prefix>.meta.txt
//
// OpenMP 並列で 128 thread フル活用。
//
// 注: 教師 binpack の psv.score は side-to-move POV、本実装の feature は BLACK POV
//     なので、WHITE 手番の局面では y の符号を反転する (キャンセル防止)。

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

void mobility_dump_cmd(Position& /*pos*/, std::istringstream& is) {
	std::string input_file;
	std::string output_prefix = "mobility_features";
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
		else std::cerr << "mobility_dump: unknown token: " << token << std::endl;
	}

	if (input_file.empty()) {
		std::cerr << "mobility_dump: input is required" << std::endl;
		return;
	}

	const size_t REC_SIZE = sizeof(Learner::PackedSfenValue);
	if (REC_SIZE != 40) {
		std::cerr << "mobility_dump: unexpected PackedSfenValue size " << REC_SIZE << std::endl;
		return;
	}

	std::ifstream ifs(input_file, std::ios::binary);
	if (!ifs) {
		std::cerr << "mobility_dump: failed to open " << input_file << std::endl;
		return;
	}
	ifs.seekg(0, std::ios::end);
	const uint64_t file_size = (uint64_t)ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	const uint64_t total_records = file_size / REC_SIZE;
	if (skip >= total_records) {
		std::cerr << "mobility_dump: skip >= total_records (" << total_records << ")" << std::endl;
		return;
	}
	uint64_t n = std::min(max_positions, total_records - skip);

	std::cout << "mobility_dump:" << std::endl
	          << "  input          = " << input_file << std::endl
	          << "  output_prefix  = " << output_prefix << std::endl
	          << "  feat_dim       = " << NUM_FEATURES << std::endl
	          << "  total_records  = " << total_records << std::endl
	          << "  skip           = " << skip << std::endl
	          << "  N to extract   = " << n << std::endl;

	if (skip > 0)
		ifs.seekg((std::streamoff)(skip * REC_SIZE), std::ios::beg);

	std::vector<Learner::PackedSfenValue> psvs(n);
	ifs.read(reinterpret_cast<char*>(psvs.data()), (std::streamsize)(n * REC_SIZE));
	if ((uint64_t)ifs.gcount() != n * REC_SIZE) {
		std::cerr << "mobility_dump: short read (" << ifs.gcount() << " bytes)" << std::endl;
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
		std::cerr << "mobility_dump: WARN Threads option (" << existing_threads
		          << ") < requested (" << max_threads << "). Clamping." << std::endl;
		max_threads = existing_threads;
	}
	std::cout << "  threads        = " << max_threads << std::endl;

	std::vector<float> X((size_t)n * NUM_FEATURES);
	std::vector<float> y(n);
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
			float feat[NUM_FEATURES];
			extract_features(pos, feat);
			std::memcpy(X.data() + (size_t)i * NUM_FEATURES, feat, sizeof(float) * NUM_FEATURES);
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
			std::memcpy(X.data() + kept * NUM_FEATURES, X.data() + i * NUM_FEATURES,
			            sizeof(float) * NUM_FEATURES);
			y[kept] = y[i];
		}
		++kept;
	}
	if (kept != n)
		std::cout << "mobility_dump: dropped " << (n - kept) << " invalid records" << std::endl;

	const std::string xpath = output_prefix + ".X.bin";
	const std::string ypath = output_prefix + ".y.bin";
	const std::string mpath = output_prefix + ".meta.txt";

	{
		std::ofstream ofs(xpath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(X.data()),
		          (std::streamsize)(sizeof(float) * kept * NUM_FEATURES));
	}
	{
		std::ofstream ofs(ypath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(y.data()),
		          (std::streamsize)(sizeof(float) * kept));
	}
	{
		std::ofstream ofs(mpath);
		ofs << "N=" << kept << "\n"
		    << "D=" << NUM_FEATURES << "\n"
		    << "board_features=" << NUM_BOARD_FEATURES << "\n"
		    << "hand_features=" << NUM_HAND_FEATURES << "\n"
		    << "dtype=float32\n"
		    << "X_shape=(N,D) row-major\n";
	}

	std::cout << "mobility_dump: wrote " << kept << " rows to " << xpath << " / " << ypath << std::endl;
}

// --- qs_consistency_cmd: sfen ↔ score 整合性チェック ---------------------------
void qs_consistency_cmd(Position& /*pos*/, std::istringstream& is) {
	std::string input_file;
	std::string output_csv = "/tmp/qs_consistency.csv";
	uint64_t max_positions = 1000;
	uint64_t skip = 0;

	std::string token;
	while (is >> token) {
		if      (token == "input")         is >> input_file;
		else if (token == "output_csv")    is >> output_csv;
		else if (token == "max_positions") is >> max_positions;
		else if (token == "skip")          is >> skip;
		else std::cerr << "qs_consistency: unknown token: " << token << std::endl;
	}

	if (input_file.empty()) {
		std::cerr << "qs_consistency: input is required" << std::endl;
		return;
	}

	const size_t REC_SIZE = sizeof(Learner::PackedSfenValue);
	std::ifstream ifs(input_file, std::ios::binary);
	if (!ifs) {
		std::cerr << "qs_consistency: failed to open " << input_file << std::endl;
		return;
	}
	ifs.seekg(0, std::ios::end);
	const uint64_t file_size = (uint64_t)ifs.tellg();
	ifs.seekg(0, std::ios::beg);
	const uint64_t total_records = file_size / REC_SIZE;
	if (skip >= total_records) {
		std::cerr << "qs_consistency: skip >= total_records (" << total_records << ")" << std::endl;
		return;
	}
	uint64_t n = std::min(max_positions, total_records - skip);
	if (skip > 0) ifs.seekg((std::streamoff)(skip * REC_SIZE), std::ios::beg);

	std::vector<Learner::PackedSfenValue> psvs(n);
	ifs.read(reinterpret_cast<char*>(psvs.data()), (std::streamsize)(n * REC_SIZE));
	ifs.close();

	std::cout << "qs_consistency: input=" << input_file
	          << " total_records=" << total_records
	          << " N=" << n << std::endl;

	std::ofstream ofs(output_csv);
	ofs << "i,gamePly,score_stm,static_eval_stm,qsearch_eval_stm,abs_diff_static,abs_diff_qsearch\n";

	auto th = Threads[0];
	Position& p = th->rootPos;

	uint64_t loaded = 0, qs_match = 0, static_match = 0;
	double sum_abs_static = 0, sum_abs_qs = 0;

	for (uint64_t i = 0; i < n; ++i) {
		const auto& psv = psvs[i];
		StateInfo si;
		if (p.set_from_packed_sfen(psv.sfen, &si, th).is_not_ok()) continue;
		++loaded;
		Value se = Eval::evaluate(p);                            // STM POV
		auto qr = Learner::qsearch(p);
		int qe = (int)qr.first;                                    // STM POV
		int sc = (int)psv.score;                                   // STM POV (per gensfen convention)
		int da = std::abs(sc - (int)se);
		int dq = std::abs(sc - qe);
		sum_abs_static += da;
		sum_abs_qs     += dq;
		if (da <= 20) ++static_match;
		if (dq <= 20) ++qs_match;
		ofs << i << "," << (int)psv.gamePly << "," << sc << "," << (int)se << "," << qe
		    << "," << da << "," << dq << "\n";
	}
	ofs.close();

	std::cout << "qs_consistency: loaded=" << loaded << "/" << n << std::endl
	          << "  mean |score - static_eval|  = " << (sum_abs_static / std::max<uint64_t>(loaded,1)) << " cp" << std::endl
	          << "  mean |score - qsearch_eval| = " << (sum_abs_qs     / std::max<uint64_t>(loaded,1)) << " cp" << std::endl
	          << "  records with |diff_static|<=20  : " << static_match << " (" << (100.0*static_match/std::max<uint64_t>(loaded,1)) << "%)" << std::endl
	          << "  records with |diff_qsearch|<=20 : " << qs_match     << " (" << (100.0*qs_match    /std::max<uint64_t>(loaded,1)) << "%)" << std::endl
	          << "  wrote " << output_csv << std::endl;
}

} // namespace HumanLike
} // namespace Eval
#endif // EVAL_LEARN
