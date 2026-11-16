#include "mnw.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

namespace {

const char* const kSectionNames[kNumMnwSections] = {
	"embed", "gamma", "pair", "pair_gamma", "scale_gamma", "w1", "b1", "w2", "b2", "value_w", "value_b",
	"policy", "policy_b", "policy_from", "policy_from_gamma", "moment_embed", "moment_gamma",
	"field", "field_gamma", "policy_field",
};

uint16_t read_u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t read_u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool fail(std::string* err, const std::string& msg) {
	if (err)
		*err = msg;
	return false;
}

// n 個の f32 がすべて有限か。NaN と Inf は指数部がすべて 1。
bool all_finite(const float* p, uint64_t n, float* max_abs) {
	uint32_t max_bits = 0;
	const uint64_t block = 1u << 14;
	for (uint64_t base = 0; base < n; base += block) {
		const uint64_t end = (n - base < block) ? n : base + block;
		uint32_t bad = 0;
		for (uint64_t i = base; i < end; ++i) {
			uint32_t w;
			std::memcpy(&w, p + i, 4);
			bad |= uint32_t((w & 0x7F800000u) == 0x7F800000u);
			const uint32_t absolute = w & 0x7FFFFFFFu;
			if (absolute > max_bits) max_bits = absolute;
		}
		if (bad)
			return false;
	}
	std::memcpy(max_abs, &max_bits, 4);
	return true;
}

// 実際の float 演算より大きい絶対上界を用いる。各加算・乗算の丸めを見込み、
// factor 2 と FLT_MAX/32 の余裕を使う。局面ごとの走査は追加しない。
bool arithmetic_is_bounded(const MnwHeader& h, const float* maxima, std::string* err) {
	const double limit = double(std::numeric_limits<float>::max()) / 32.0;
	const double ntokens = 81 + 2 * 38; // BoardView が受ける最大数。合法局面の 40 枚だけには限定しない。
	auto within = [&](double bound, const char* stage) {
		return std::isfinite(bound) && bound <= limit
		       ? true : fail(err, std::string("f32 中間値の安全上界を超える: ") + stage);
	};
	double powers[4] = {};
	double power = 1.0;
	for (unsigned k = 0; k < h.order; ++k) {
		power *= double(maxima[kSecEmbed]);
		powers[k] = 2.0 * ntokens * power;
		if (!within(powers[k], "power sum")) return false;
	}
	double elementary[5] = { 1.0 };
	for (unsigned k = 1; k <= h.order; ++k) {
		double sum = 0.0;
		for (unsigned j = 1; j <= k; ++j) {
			sum += 2.0 * elementary[k - j] * powers[j - 1];
			if (!within(sum, "Newton")) return false;
		}
		elementary[k] = 2.0 * sum / double(k);
		if (!within(elementary[k], "elementary polynomial")
		    || !within(2.0 * h.dim * elementary[k] * elementary[k], "power RMS square sum")) return false;
	}
	double feature = 2.0 * std::sqrt(double(h.dim)) * maxima[kSecGamma];
	if (h.pair_dim) {
		const double pairs = ntokens * (ntokens - 1.0) / 2.0;
		const double sum = 2.0 * pairs * maxima[kSecPair];
		if (!within(sum, "pair sum") || !within(2.0 * h.pair_dim * sum * sum, "pair RMS square sum")) return false;
		const double pair_feature = 2.0 * std::sqrt(double(h.pair_dim)) * maxima[kSecPairGamma];
		if (pair_feature > feature) feature = pair_feature;
	}
	if (!within(feature, "normalized features")) return false;
	const double hidden = 2.0 * (double(maxima[kSecB1]) + double(h.feature_len) * maxima[kSecW1] * feature);
	if (!within(hidden, "W1 and bias")) return false;
	double head = hidden;
	if (h.hidden2) {
		head = 2.0 * (double(maxima[kSecB2]) + double(h.hidden) * maxima[kSecW2] * hidden);
		if (!within(head, "W2 and bias")) return false;
	}
	return within(2.0 * (double(maxima[kSecValueB]) + double(h.head_width) * maxima[kSecValueW] * head), "value row");
}

} // namespace

bool parse_mnw_header(const uint8_t* b, uint64_t file_bytes, MnwHeader* out, std::string* err) {
	if (file_bytes < kMnwHeaderBytes)
		return fail(err, "ファイルがヘッダー(" + std::to_string(kMnwHeaderBytes) + " バイト)より短い: " + std::to_string(file_bytes) + " バイト");

	MnwHeader h;
	const uint32_t magic = read_u32(b);
	if (magic != kMnwMagic) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "0x%08x", magic);
		return fail(err, std::string("MNW1 ではない(magic ") + buf + ")");
	}
	h.version = read_u16(b + 4);
	if (h.version != kMnwVersion)
		return fail(err, "MNW1 の版が違う: " + std::to_string(h.version) + "(この core が読むのは " + std::to_string(kMnwVersion) + " だけ)");

	h.dim        = read_u16(b + 6);
	h.order      = read_u16(b + 8);
	h.hidden     = read_u16(b + 10);
	h.pair_dim   = read_u16(b + 12);
	h.hidden2    = read_u16(b + 14);
	h.moment_dim = read_u16(b + 16);
	h.field_dim  = read_u16(b + 18);
	h.pair_lse   = read_u16(b + 20);
	h.flags      = read_u16(b + 22);
	h.tokens      = read_u32(b + 24);
	h.policy_size = read_u32(b + 28);
	h.pair_rows   = read_u32(b + 32);
	h.file_bytes  = file_bytes;

	if (h.flags & ~kFlagKnownMask)
		return fail(err, "flags に未知のビットがある: " + std::to_string(h.flags));
	h.keep_scale  = (h.flags & kFlagKeepScale) != 0;
	h.features    = (h.flags >> 1) & 3;
	h.nnue        = (h.flags & kFlagNnue) != 0;
	h.split_sides = (h.flags & kFlagSplit) != 0;

	if (h.features > kFeaturesHalfKp)
		return fail(err, "未知の特徴空間 " + std::to_string(h.features));

	// 特徴空間から決まる行数(token.rs の Features::token_count と pair_rows)。
	const uint32_t kp_tokens     = 90 + 9 * 2 * 81 + 2 * 81;
	const uint32_t halfkp_tokens = 81 * (90 + 9 * 2 * 81) + 1;
	const uint32_t want_tokens   = h.features == kFeaturesTokens ? kNumTokens : (h.features == kFeaturesKp ? kp_tokens : halfkp_tokens);
	const uint32_t want_pairs    = h.features == kFeaturesTokens ? kNumPairs : 0;
	if (h.pair_rows != want_pairs)
		return fail(err, "pair_rows が特徴空間と合わない: ファイル " + std::to_string(h.pair_rows) + "、期待 " + std::to_string(want_pairs));
	if (h.tokens != want_tokens)
		return fail(err, "tokens が特徴空間と合わない: ファイル " + std::to_string(h.tokens) + "、期待 " + std::to_string(want_tokens));
	if (h.policy_size != kPolicySize)
		return fail(err, "policy_size が違う: ファイル " + std::to_string(h.policy_size) + "、期待 " + std::to_string(kPolicySize));
	if (h.order < 1 || h.order > kMaxOrder)
		return fail(err, "order " + std::to_string(h.order) + " は 1 から " + std::to_string(kMaxOrder) + " の外");
	if (h.dim == 0 || h.hidden == 0)
		return fail(err, "次元が 0(dim " + std::to_string(h.dim) + "、hidden " + std::to_string(h.hidden) + ")");
	if (h.pair_lse > h.pair_dim)
		return fail(err, "pair_lse " + std::to_string(h.pair_lse) + " が pair_dim " + std::to_string(h.pair_dim) + " を超える");
	if (h.split_sides && h.features != kFeaturesTokens)
		return fail(err, "split は Tokens 空間だけで定義されている");
	if (h.split_sides && h.nnue)
		return fail(err, "split と nnue は同時に立たない");

	// 形から決まる値(params.rs の Arch)。
	const uint64_t power_groups = h.split_sides ? 2 : 1;
	h.head_width = h.hidden2 > 0 ? h.hidden2 : h.hidden;
	h.moment_len = uint64_t(h.moment_dim) * (uint64_t(h.moment_dim) + 1) / 2;
	h.scale_len  = 0;
	if (h.keep_scale)
		h.scale_len = power_groups * h.order + (h.pair_dim - h.pair_lse > 0 ? 1 : 0) + (h.moment_dim > 0 ? 1 : 0) + (h.field_dim > 0 ? 1 : 0);
	h.block_len = power_groups * h.order * h.dim + h.pair_dim + h.moment_len + uint64_t(kMnwFieldSquares) * h.field_dim + h.scale_len;
	h.feature_len = 2 * h.block_len;

	// 全配列の位置。値は u16 と u32 の積なので 64 ビットに収まるが、桁あふれを検出する形で書いて、将来の形の追加に備える。
	const uint64_t head = h.head_width;
	uint64_t counts[kNumMnwSections];
	bool ok = true;
	auto mul = [&](uint64_t a, uint64_t c) { uint64_t r = 0; ok = ok && checked_mul(a, c, &r); return r; };
	counts[kSecEmbed]           = mul(h.tokens, h.dim);
	counts[kSecGamma]           = mul(h.order, h.dim);
	counts[kSecPair]            = mul(h.pair_rows, h.pair_dim);
	counts[kSecPairGamma]       = h.pair_dim;
	counts[kSecScaleGamma]      = h.scale_len;
	counts[kSecW1]              = mul(h.hidden, h.feature_len);
	counts[kSecB1]              = h.hidden;
	counts[kSecW2]              = mul(h.hidden2, h.hidden);
	counts[kSecB2]              = h.hidden2;
	counts[kSecValueW]          = head;
	counts[kSecValueB]          = 1;
	counts[kSecPolicy]          = mul(h.policy_size, head);
	counts[kSecPolicyB]         = h.policy_size;
	counts[kSecPolicyFrom]      = mul(h.dim, head);
	counts[kSecPolicyFromGamma] = h.dim;
	counts[kSecMomentEmbed]     = mul(h.tokens, h.moment_dim);
	counts[kSecMomentGamma]     = h.moment_len;
	counts[kSecField]           = mul(mul(h.tokens, kMnwFieldSquares), h.field_dim);
	counts[kSecFieldGamma]      = h.field_dim;
	counts[kSecPolicyField]     = mul(kMoveChannels, h.field_dim);
	if (!ok)
		return fail(err, "配列の大きさの計算が 64 ビットに収まらない");

	uint64_t cursor = kMnwHeaderBytes;
	for (int i = 0; i < kNumMnwSections; ++i) {
		uint64_t bytes = 0;
		if (!checked_mul(counts[i], 4, &bytes) || !checked_add(cursor, bytes, &cursor))
			return fail(err, std::string("配列 ") + kSectionNames[i] + " の位置の計算が 64 ビットに収まらない");
		h.section[i].count = counts[i];
		h.section[i].offset = cursor - bytes;
	}

	// 確保の前に、ファイルの大きさと照合する。
	if (cursor != file_bytes) {
		if (file_bytes < cursor)
			return fail(err, "ファイルが短い(切り詰め): 形から期待される大きさ " + std::to_string(cursor) + " バイト、実際 " + std::to_string(file_bytes) + " バイト");
		return fail(err, "ファイルの末尾に余りがある: 形から期待される大きさ " + std::to_string(cursor) + " バイト、実際 " + std::to_string(file_bytes) + " バイト");
	}

	*out = h;
	return true;
}

namespace {

// 呼び出し側が開いた同じ fd から、大きさとヘッダーを読む。
bool read_header_fd(int fd, MnwHeader* out, std::string* err) {
	struct stat st;
	if (::fstat(fd, &st) != 0)
		return fail(err, std::strerror(errno));
	if (!S_ISREG(st.st_mode) || st.st_size < 0)
		return fail(err, "通常のファイルではない");
	uint8_t head[kMnwHeaderBytes] = {};
	const size_t want = uint64_t(st.st_size) < kMnwHeaderBytes ? size_t(st.st_size) : size_t(kMnwHeaderBytes);
	ssize_t got;
	do {
		got = ::pread(fd, head, want, 0);
	} while (got < 0 && errno == EINTR);
	if (got < 0 || uint64_t(got) != want)
		return fail(err, "ヘッダーを読めない");

	return parse_mnw_header(head, uint64_t(st.st_size), out, err);
}

} // namespace

bool read_mnw_header(const std::string& path, MnwHeader* out, std::string* err) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fail(err, path + ": " + std::strerror(errno));
	const bool ok = read_header_fd(fd, out, err);
	::close(fd);
	if (!ok && err)
		*err = path + ": " + *err;
	return ok;
}

const char* mnw_unsupported_reason(const MnwHeader& h) {
	if (h.features != kFeaturesTokens)
		return "KP と HalfKP の特徴空間は v1 では未対応";
	if (h.nnue)
		return "nnue 算術(RMS 無し、clamp、線形出力)は v1 では未対応";
	if (h.split_sides)
		return "split(power sum を所有者で分ける)は v1 では未対応";
	if (h.keep_scale)
		return "ks(各ブロックの ln(rms) を特徴に足す)は v1 では未対応";
	if (h.pair_lse > 0)
		return "lse(pair の末尾を soft max にする)は v1 では未対応";
	if (h.moment_dim > 0)
		return "m(第 2 モーメント)は v1 では未対応";
	if (h.field_dim > 0)
		return "f(升ごとの場)は v1 では未対応";
	return nullptr;
}

std::string mnw_shape_name(const MnwHeader& h) {
	std::string s;
	if (h.nnue)
		s += "NNUE_";
	s += h.features == kFeaturesTokens ? "token" : (h.features == kFeaturesKp ? "kp" : "halfkp");
	s += "_pair" + std::to_string(h.pair_dim);
	s += "_d" + std::to_string(h.dim);
	s += "_o" + std::to_string(h.order);
	s += "_h" + std::to_string(h.hidden);
	if (h.hidden2 > 0)
		s += "_h2" + std::to_string(h.hidden2);
	if (h.moment_dim > 0)
		s += "_m" + std::to_string(h.moment_dim);
	if (h.field_dim > 0)
		s += "_f" + std::to_string(h.field_dim);
	if (h.pair_lse > 0)
		s += "_lse" + std::to_string(h.pair_lse);
	if (h.keep_scale)
		s += "_ks";
	if (h.split_sides)
		s += "_split";
	return s;
}

MnwWeights::MnwWeights(MnwWeights&& o) noexcept { *this = std::move(o); }

MnwWeights& MnwWeights::operator=(MnwWeights&& o) noexcept {
	if (this != &o) {
		release();
		++revision_;
		++o.revision_; // 移動元に結び付いた評価器も使用できなくする。
		header_ = o.header_;
		embed_ = std::move(o.embed_);
		gamma_ = std::move(o.gamma_);
		pair_gamma_ = std::move(o.pair_gamma_);
		w1_ = std::move(o.w1_);
		b1_ = std::move(o.b1_);
		w2_ = std::move(o.w2_);
		b2_ = std::move(o.b2_);
		value_w_ = std::move(o.value_w_);
		pair_owned_ = std::move(o.pair_owned_);
		pair_ = o.pair_;
		value_b_ = o.value_b_;
		map_base_ = o.map_base_;
		map_bytes_ = o.map_bytes_;
		o.header_ = MnwHeader{};
		o.value_b_ = 0.0f;
		o.pair_ = nullptr;
		o.map_base_ = nullptr;
		o.map_bytes_ = 0;
	}
	return *this;
}

MnwWeights::~MnwWeights() { release(); }

void MnwWeights::release() {
	if (map_base_ != nullptr)
		::munmap(map_base_, map_bytes_);
	map_base_ = nullptr;
	map_bytes_ = 0;
	pair_ = nullptr;
	pair_owned_.reset();
	embed_.reset();
	gamma_.reset();
	pair_gamma_.reset();
	w1_.reset();
	b1_.reset();
	w2_.reset();
	b2_.reset();
	value_w_.reset();
}

uint64_t MnwWeights::owned_bytes() const {
	const uint64_t n = embed_.size() + gamma_.size() + pair_gamma_.size() + w1_.size() + b1_.size() + w2_.size() + b2_.size() + value_w_.size() + pair_owned_.size();
	return n * 4;
}

bool MnwWeights::load(const std::string& path, const MnwLoadOptions& opt, MnwWeights* out, std::string* err) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fail(err, path + ": " + std::strerror(errno));
	MnwHeader h;
	if (!read_header_fd(fd, &h, err)) {
		::close(fd);
		if (err) *err = path + ": " + *err;
		return false;
	}
	if (const char* why = mnw_unsupported_reason(h)) {
		::close(fd);
		return fail(err, path + ": 対象外の形 " + mnw_shape_name(h) + ": " + why);
	}
	// mmap とコピーのいずれでも、この開いた fd で大きさを確かめてから確保する。
	if (h.file_bytes > (uint64_t(8) << 30) || h.file_bytes > SIZE_MAX) {
		::close(fd);
		return fail(err, path + ": ファイルがメモリ上限(8 GiB)を超える");
	}

	int flags = MAP_PRIVATE;
	if (opt.populate)
		flags |= MAP_POPULATE;
	void* base = ::mmap(nullptr, h.file_bytes, PROT_READ, flags, fd, 0);
	::close(fd);
	if (base == MAP_FAILED)
		return fail(err, path + ": mmap に失敗: " + std::strerror(errno));
	const uint8_t* bytes = static_cast<const uint8_t*>(base);

	auto at = [&](int id) { return reinterpret_cast<const float*>(bytes + h.section[id].offset); };

	MnwWeights w;
	w.header_ = h;

	// policy を含め、ファイル中の全配列の NaN と Inf を拒否する。
	float maxima[kNumMnwSections] = {};
	for (int id = 0; id < kNumMnwSections; ++id) {
		if (h.section[id].count != 0 && !all_finite(at(id), h.section[id].count, &maxima[id])) {
			::munmap(base, h.file_bytes);
			return fail(err, path + ": 配列 " + kSectionNames[id] + " に NaN か Inf がある");
		}
	}

	if (!arithmetic_is_bounded(h, maxima, err)) {
		::munmap(base, h.file_bytes);
		if (err) *err = path + ": " + *err;
		return false;
	}

	bool ok = true;
	auto copy = [&](AlignedBuffer<float>& dst, int id) {
		if (!dst.allocate(h.section[id].count)) {
			ok = false;
			return;
		}
		if (h.section[id].count != 0)
			std::memcpy(dst.data(), at(id), h.section[id].count * 4);
	};
	copy(w.embed_, kSecEmbed);
	copy(w.gamma_, kSecGamma);
	copy(w.pair_gamma_, kSecPairGamma);
	copy(w.w1_, kSecW1);
	copy(w.b1_, kSecB1);
	copy(w.w2_, kSecW2);
	copy(w.b2_, kSecB2);
	copy(w.value_w_, kSecValueW);
	std::memcpy(&w.value_b_, at(kSecValueB), 4);

	if (h.section[kSecPair].count != 0) {
		if (opt.mmap_pair) {
			w.pair_ = at(kSecPair);
			// pair の行は順序も局所性も無く読まれる。先読みは無駄。
			const uint64_t page = 4096;
			const uint64_t start = (h.section[kSecPair].offset + page - 1) / page * page;
			const uint64_t end = (h.section[kSecPair].offset + h.section[kSecPair].count * 4) / page * page;
			if (end > start)
				::madvise(const_cast<uint8_t*>(bytes) + start, end - start, MADV_RANDOM);
		} else {
			const uint64_t n = h.section[kSecPair].count;
			const size_t align = (n * 4 >= (4u << 20)) ? (2u << 20) : 64;
			if (!w.pair_owned_.allocate(n, align)) {
				ok = false;
			} else {
				if (align == (2u << 20))
					::madvise(w.pair_owned_.data(), (n * 4 + align - 1) / align * align, MADV_HUGEPAGE);
				std::memcpy(w.pair_owned_.data(), at(kSecPair), n * 4);
				w.pair_ = w.pair_owned_.data();
			}
		}
	}

	if (!ok) {
		::munmap(base, h.file_bytes);
		return fail(err, path + ": メモリを確保できない");
	}

	if (w.pair_ != nullptr && opt.mmap_pair) {
		w.map_base_ = base;
		w.map_bytes_ = h.file_bytes;
	} else {
		::munmap(base, h.file_bytes);
	}
	*out = std::move(w);
	return true;
}

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu
