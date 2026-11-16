#include "../../config.h"

#if defined(EVAL_MANAKA)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "manaka_net.h"

#if defined(MANAKA_DEV_MNW1)
#include "core/eval_f32.h"
#include "core/mnw.h"
#endif

// ネットワークの読み込み口。ファイルの先頭で形式を見分ける。
//
//   nn.bin (ModelType=SYMq16) : 整数評価。manaka_net_q16.cpp。
//   MNW1 (.mnw)               : f32 のままの開発用の形式で、公開物ではない。ビルドに -DMANAKA_DEV_MNW1
//                               (make の EXTRA_CPPFLAGS) を付けたときだけ読む。f32 の参照値と突き合わせる
//                               ための経路で、アキュムレータは使わず毎回盤から全計算する。
//
// どちらの形式でもない、あるいは壊れたファイルを、別の形式として救済することはしない。

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

namespace {

constexpr uint32_t kMnwMagic    = 0x31574E4Du; // "MNW1"
constexpr uint32_t kNnueVersion = 0x7AF32F16u; // nn.bin の先頭 4 バイト。NNUE と同じ

#if defined(MANAKA_DEV_MNW1)

class MnwSession final : public Session {
public:
	explicit MnwSession(std::unique_ptr<ManakaCore::EvaluatorF32> evaluator) : evaluator_(std::move(evaluator)) {}

	bool incremental() const override { return false; }

	bool rebuild(const ManakaCore::BoardView& board, Accumulator&, float* pre, std::string* err) override {
		return full(board, pre, err);
	}
	bool update(const ManakaCore::BoardView& board, const Accumulator&, Accumulator&, float* pre, std::string* err) override {
		return full(board, pre, err);
	}
	bool score(const ManakaCore::BoardView& board, const Accumulator&, float* pre, std::string* err) override {
		return full(board, pre, err);
	}

private:
	bool full(const ManakaCore::BoardView& board, float* pre, std::string* err) {
		ManakaCore::EvaluatorF32::Result r;
		if (!evaluator_->evaluate(board, &r))
		{
			if (err)
				*err = "the board cannot be evaluated";
			return false;
		}
		// f32 の評価は、有限だが巨大な重みで積和があふれると NaN や無限大を返しうる。この翻訳単位は
		// 厳密にビルドされるので、ここで拒否する (0 で代替しない)。
		if (!std::isfinite(r.pre))
		{
			if (err)
				*err = "non-finite pre";
			return false;
		}
		*pre = r.pre;
		return true;
	}

	std::unique_ptr<ManakaCore::EvaluatorF32> evaluator_;
};

class MnwNetwork final : public Network {
public:
	const OutputMap& output_map() const override { return map_; }

	std::unique_ptr<Session> new_session() const override {
		std::string err;
		auto evaluator = ManakaCore::EvaluatorF32::create(&weights_, &err);
		if (!evaluator)
			return nullptr;
		return std::unique_ptr<Session>(new MnwSession(std::move(evaluator)));
	}

	std::string describe() const override {
		const ManakaCore::MnwHeader& h = weights_.header();
		return "MNW1 (development, f32) " + ManakaCore::mnw_shape_name(h) + ", "
		     + std::to_string(h.file_bytes) + " bytes";
	}

	ManakaCore::MnwWeights weights_;
	OutputMap              map_; // MNW1 は自己記述ではないので既定値 (1512.173 * 0.9、0.9999)
};

#endif // defined(MANAKA_DEV_MNW1)

} // namespace

std::unique_ptr<Network> load_network_mnw(const std::string& path, bool require_checksum, std::string* err) {
	if (require_checksum)
	{
		// MNW1 にはチェックサムが無い。内容を検証できない形式を、検証を求められて読まない。
		*err = "checksum required but the file is MNW1 (development format), which has no checksum";
		return nullptr;
	}
#if defined(MANAKA_DEV_MNW1)
	auto net = std::unique_ptr<MnwNetwork>(new MnwNetwork());
	ManakaCore::MnwLoadOptions options; // pair 表は mmap のまま参照する
	if (!ManakaCore::MnwWeights::load(path, options, &net->weights_, err))
		return nullptr;
	return net;
#else
	(void)path;
	*err = "MNW1 (.mnw) is a development format and this build does not read it; "
	       "rebuild with EXTRA_CPPFLAGS=-DMANAKA_DEV_MNW1, or use nn.bin (ModelType=SYMq16)";
	return nullptr;
#endif
}

std::unique_ptr<Network> load_network(const std::string& path, bool require_checksum, std::string* err) {
	std::FILE* fp = std::fopen(path.c_str(), "rb");
	if (fp == nullptr) {
		*err = "cannot open the file";
		return nullptr;
	}
	uint8_t head[8] = {};
	const size_t got = std::fread(head, 1, sizeof(head), fp);
	std::fclose(fp);
	if (got < 4) {
		*err = "the file is shorter than 4 bytes";
		return nullptr;
	}

	uint32_t first;
	std::memcpy(&first, head, sizeof(first)); // リトルエンディアンのホストを前提 (core/common.h が static_assert する)

	if (first == kMnwMagic)
		return load_network_mnw(path, require_checksum, err);

	if (first == kNnueVersion)
		return load_network_q16(path, require_checksum, err); // ModelType の判定と全検査は読み込み側がする

	*err = "unknown file format (neither MNW1 nor nn.bin)";
	return nullptr;
}

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)
