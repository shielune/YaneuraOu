#include "q16.h"
#include "tokens.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__SSE4_2__) && defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace YaneuraOu { namespace Eval { namespace ManakaCore {
namespace {
#define Q16_BASE_TAIL ";Features=Tokens[128x2],Pair[16],Network=Dense[1<-512](ReLU[512](Dense[512<-800](RmsNormScaled[384](Newton[3](PowerSum[3](InputSlice[384(0:384)]))))"
#define Q16_H2_TAIL ";Features=Tokens[128x2],Pair[16],Network=Dense[1<-256](ReLU[256](Dense[256<-512](ReLU[512](Dense[512<-800](RmsNormScaled[384](Newton[3](PowerSum[3](InputSlice[384(0:384)]))))))"
constexpr char base_arch[] = "ModelType=SYMq16" Q16_BASE_TAIL;
constexpr char h2_arch[] = "ModelType=SYMq16" Q16_H2_TAIL;
// SYMq16c = SYMq16 plus a section 7 trailer: u32(hash+7), u32 CRC-32C of every byte before it.
constexpr char basec_arch[] = "ModelType=SYMq16c" Q16_BASE_TAIL;
constexpr char h2c_arch[] = "ModelType=SYMq16c" Q16_H2_TAIL;
#undef Q16_BASE_TAIL
#undef Q16_H2_TAIL
constexpr size_t kTrailerBytes = 8;
struct Variant {
	const char* arch; uint32_t len; bool hidden2; bool checksummed; uint64_t bytes;
};
// The file length is fixed by the architecture: the header string length plus the section sizes.
constexpr Variant kVariants[] = {
	{base_arch, sizeof(base_arch) - 1, false, false, 89299080},
	{h2_arch, sizeof(h2_arch) - 1, true, false, 89564328},
	{basec_arch, sizeof(basec_arch) - 1, false, true, 89299080 + (sizeof(basec_arch) - sizeof(base_arch)) + kTrailerBytes},
	{h2c_arch, sizeof(h2c_arch) - 1, true, true, 89564328 + (sizeof(h2c_arch) - sizeof(h2_arch)) + kTrailerBytes},
};
static_assert(kVariants[2].bytes == 89299089 && kVariants[3].bytes == 89564337, "SYMq16c lengths");

// CRC-32C, reflected 0x82F63B78. The step functions carry the raw register (no init/xorout) so the loader can
// feed the file in the order it reads it.
struct CrcTables {
	uint32_t t[8][256];
	constexpr CrcTables() : t() {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
			t[0][i] = c;
		}
		for (uint32_t i = 0; i < 256; ++i)
			for (int k = 1; k < 8; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 255];
	}
};
constexpr CrcTables kCrc{};
uint32_t crc32c_sw(uint32_t c, const uint8_t* p, size_t n) {
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint32_t a, b;
		std::memcpy(&a, p + i, 4); std::memcpy(&b, p + i + 4, 4);
		a ^= c;
		c = kCrc.t[7][a & 255] ^ kCrc.t[6][(a >> 8) & 255] ^ kCrc.t[5][(a >> 16) & 255] ^ kCrc.t[4][a >> 24]
		  ^ kCrc.t[3][b & 255] ^ kCrc.t[2][(b >> 8) & 255] ^ kCrc.t[1][(b >> 16) & 255] ^ kCrc.t[0][b >> 24];
	}
	for (; i < n; ++i) c = (c >> 8) ^ kCrc.t[0][(c ^ p[i]) & 255];
	return c;
}
#if defined(__SSE4_2__) && defined(__x86_64__)
constexpr bool kHasHardwareCrc = true;
uint32_t crc32c_hw(uint32_t c, const uint8_t* p, size_t n) {
	uint64_t s = c;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); s = _mm_crc32_u64(s, w); }
	uint32_t r = static_cast<uint32_t>(s);
	for (; i < n; ++i) r = _mm_crc32_u8(r, p[i]);
	return r;
}
#else
constexpr bool kHasHardwareCrc = false;
uint32_t crc32c_hw(uint32_t c, const uint8_t* p, size_t n) { return crc32c_sw(c, p, n); }
#endif
uint32_t crc32c_step(uint32_t c, const uint8_t* p, size_t n, bool hardware) {
	return hardware && kHasHardwareCrc ? crc32c_hw(c, p, n) : crc32c_sw(c, p, n);
}

// True when some int16 in p[0..elems) is -32768. p may be at any alignment (SYMq16c shifts the arrays by one byte).
bool any_min16(const uint8_t* p, size_t elems) {
	size_t i = 0;
#if defined(__AVX2__)
	const __m256i minimum = _mm256_set1_epi16(static_cast<short>(0x8000));
	__m256i found = _mm256_setzero_si256();
	for (; i + 16 <= elems; i += 16)
		found = _mm256_or_si256(found, _mm256_cmpeq_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 2 * i)), minimum));
	if (!_mm256_testz_si256(found, found)) return true;
#endif
	for (; i < elems; ++i) {
		uint16_t v;
		std::memcpy(&v, p + 2 * i, 2);
		if (v == 0x8000u) return true;
	}
	return false;
}
bool fail(std::string* e, const char* s) { if (e) *e = s; return false; }
uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
// Scalar token collection (reference path): sorted ids per frame.
bool get_tokens(const BoardView& b, uint16_t ids[2][kMaxTokensPerFrame], std::string* e) {
	if (!validate_board(b, e)) return false;
	if (collect_tokens(b, ids) != 40) return fail(e, "SYMq16 requires exactly 40 tokens");
	for (int f = 0; f < 2; ++f) {
		std::sort(ids[f], ids[f] + 40);
		for (int i = 0; i < 40; ++i)
			if (ids[f][i] >= kTokenSpace || (i && ids[f][i] == ids[f][i-1]))
				return fail(e, "invalid or duplicate token");
	}
	return true;
}
bool quant(double x, uint32_t& clamps, int16_t& out) {
	if (!std::isfinite(x)) return false;
	double y = std::round(x);
	if (y > 32767 || y < -32767) { ++clamps; y = std::max(-32767.0, std::min(32767.0, y)); }
	out = static_cast<int16_t>(y);
	return true;
}
}

int q16_token_diff(const uint64_t* a, const uint64_t* b) {
	int n = 0;
	for (int w = 0; w < kQ16TokenWords; ++w) n += __builtin_popcountll(a[w] & ~b[w]);
	return n;
}
void q16_token_bits(const uint16_t* ids, uint64_t* bits) {
	std::memset(bits, 0, sizeof(uint64_t) * kQ16TokenWords);
	for (int i = 0; i < 40; ++i) bits[ids[i] >> 6] |= uint64_t(1) << (ids[i] & 63);
}

int64_t q16_dot(const int16_t* a, const int16_t* b, int n, bool simd) {
	int64_t sum = 0;
	int i = 0;
#if defined(__AVX2__)
	if (simd) {
		__m256i lo = _mm256_setzero_si256(), hi = lo;
		for (; i + 16 <= n; i += 16) {
			// Each pair sum is bounded by 2*32767^2 < INT32_MAX.
			// Widen immediately: no accumulated i32 partial sum can overflow.
			const auto p = _mm256_madd_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i)),
			                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+i)));
			lo = _mm256_add_epi64(lo, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(p)));
			hi = _mm256_add_epi64(hi, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(p, 1)));
		}
		alignas(32) int64_t lanes[4];
		_mm256_store_si256(reinterpret_cast<__m256i*>(lanes), _mm256_add_epi64(lo, hi));
		for (auto v : lanes) sum += v;
	}
#else
	(void)simd;
#endif
	for (; i < n; ++i) sum += int64_t(a[i]) * b[i];
	return sum;
}
uint32_t q16_crc32c(const void* data, size_t bytes, bool hardware) {
	return ~crc32c_step(0xFFFFFFFFu, static_cast<const uint8_t*>(data), bytes, hardware);
}
bool q16_crc32c_has_hardware() { return kHasHardwareCrc; }

Q16Network::~Q16Network() { reset(); }
void Q16Network::reset() {
	if (mapping_) munmap(mapping_, bytes_);
	mapping_ = nullptr; bytes_ = 0;
	hidden2_ = false; checksum_verified_ = false;
	small_.reset();
	embed_ = w1_ = w2_ = nullptr; pair_ = nullptr;
	m_ = inv1_ = b1_ = a1_ = inv2_ = b2_ = vw_ = vb_ = nullptr;
	plan1_ = DensePlan(); plan2_ = DensePlan();
}

// ---------------------------------------------------------------------------------------------------------------
// Dense layers (W1, W2): exact int64 results from int16 inputs and int16 weights.
//
// The reference widens every vpmaddwd result to i64 before adding it. That costs three shuffles per 16 MACs and makes
// W1 about twice as slow as streaming the weights from L2. The fast kernel adds N consecutive vpmaddwd results in i32
// and widens once per N. It is exact only if no lane of the i32 partial sum can overflow, so the weights are sorted at
// load time into groups of 4 rows that share one N, chosen so that for every chunk of N iterations
//     max over lanes of ( sum of |w| of the products that land in that lane ) * 32767 <= INT32_MAX
// holds. Inputs (x, hidden activations) are at most 32767 in magnitude (the quantizers clamp), so the bound is a
// proof for every possible input, not a statement about the calibration set. A lane gets two products per vpmaddwd
// (adjacent columns), so lane j of iteration i covers columns 16i+2j and 16i+2j+1.
// Rows whose weights fit no N > 1 use N = 1 (a single vpmaddwd result never overflows: 2*32767^2 < 2^31).
// ---------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kChunkClasses[6] = {8, 6, 4, 3, 2, 1};
}
bool Q16Network::build_plan(const int16_t* w, int rows, int cols, DensePlan& plan) {
	plan = DensePlan();
	plan.rows = rows; plan.cols = cols;
	const int iters = cols / 16;
	std::vector<std::array<bool, 6>> ok(rows);
	std::vector<int> cls(rows, 5);
	std::vector<uint32_t> lane(size_t(iters) * 8);
	for (int r = 0; r < rows; ++r) {
		const int16_t* row = w + size_t(r) * cols;
		for (int it = 0; it < iters; ++it)
			for (int j = 0; j < 8; ++j)
				lane[size_t(it)*8+j] = uint32_t(std::abs(int(row[it*16+2*j]))) + uint32_t(std::abs(int(row[it*16+2*j+1])));
		for (int ci = 0; ci < 6; ++ci) {
			const int N = kChunkClasses[ci];
			bool good = true;
			for (int c0 = 0; c0 < iters && good; c0 += N) {
				uint64_t s[8] = {};
				for (int it = c0; it < std::min(c0 + N, iters); ++it)
					for (int j = 0; j < 8; ++j) s[j] += lane[size_t(it)*8+j];
				for (int j = 0; j < 8; ++j)
					if (s[j] * 32767u > uint64_t(INT32_MAX)) good = false;
			}
			ok[r][ci] = good;
		}
		ok[r][5] = true;
		int c = 0; while (!ok[r][c]) ++c;
		cls[r] = c;
	}
	std::vector<int> list[6];
	for (int r = 0; r < rows; ++r) list[cls[r]].push_back(r);
	std::vector<int> order; // rows in slot order
	std::vector<std::pair<int,int>> runs; // (class index, groups)
	for (int ci = 0; ci < 6; ++ci) {
		std::vector<int>& l = list[ci];
		if (ci < 5) {
			// Rows that do not fill a group of 4 move to the next class they are safe in.
			const size_t left = l.size() % 4;
			for (size_t k = 0; k < left; ++k) {
				const int r = l.back(); l.pop_back();
				int cj = ci + 1; while (!ok[r][cj]) ++cj;
				list[cj].push_back(r);
			}
			std::sort(l.begin(), l.end());
		}
		const size_t groups = l.size() / 4;
		if (groups) runs.push_back({ci, int(groups)});
		for (size_t k = 0; k < groups * 4; ++k) order.push_back(l[k]);
		if (ci == 5) {
			plan.tail_rows = int(l.size() - groups * 4);
			for (size_t k = groups * 4; k < l.size(); ++k) order.push_back(l[k]);
		}
	}
	if (int(order.size()) != rows) return false;
	if (!plan.packed.allocate(size_t(rows) * cols, 64)) return false;
	plan.row_of_slot.resize(rows);
	for (int s = 0; s < rows; ++s) {
		plan.row_of_slot[s] = uint16_t(order[s]);
		std::memcpy(plan.packed.data() + size_t(s) * cols, w + size_t(order[s]) * cols, size_t(cols) * 2);
	}
	for (auto& r : runs) plan.runs.push_back({kChunkClasses[r.first], r.second});
	return true;
}

bool Q16Network::load(const std::string& path, std::string* error, bool require_checksum) {
	// Refuse reload: existing accumulator identity must remain valid.
	if (mapping_) return fail(error, "network is already loaded");
	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0) return fail(error, "cannot open SYMq16 file");
	struct stat st{};
	bool known_length = false;
	if (fstat(fd, &st) == 0)
		for (const Variant& v : kVariants) known_length |= static_cast<uint64_t>(st.st_size) == v.bytes;
	if (!known_length) { close(fd); return fail(error, "invalid SYMq16 file length"); }
	bytes_ = static_cast<size_t>(st.st_size);
	mapping_ = mmap(nullptr, bytes_, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
	if (mapping_ == MAP_FAILED) { mapping_ = nullptr; bytes_ = 0; return fail(error, "mmap failed"); }
	auto reject = [&](const std::string& message) { reset(); if (error) *error = message; return false; };
	const auto* data = static_cast<const uint8_t*>(mapping_);
	if (bytes_ < 12 || u32(data) != 0x7AF32F16) return reject("wrong SYMq16 version");
	uint32_t hash = u32(data+4), len = u32(data+8);
	const Variant* variant = nullptr;
	for (const Variant& v : kVariants)
		if (len == v.len && bytes_ >= 12 + size_t(len) && std::memcmp(data+12, v.arch, len) == 0) variant = &v;
	if (!variant) return reject("unsupported architecture/flags");
	uint32_t computed = 0x811C9DC5;
	for (uint32_t i=0; i<len; ++i) computed = (computed ^ data[12+i]) * 0x01000193u;
	if (hash != computed || bytes_ != variant->bytes) return reject("architecture hash/length mismatch");
	if (require_checksum && !variant->checksummed)
		return reject("checksum required but the file is ModelType=SYMq16 without a checksum trailer");
	hidden2_ = variant->hidden2;
	const bool want_crc = variant->checksummed;

	// Everything except the pair table (1.4 MB) is copied to aligned memory: SYMq16c's longer header puts the
	// arrays at odd file offsets, and int16/float arrays must not be read through misaligned pointers.
	// The pair table stays in the mapping and is only read with memcpy.
	if (!small_.allocate(2u << 20, 64)) return reject("out of memory");
	size_t cursor = 0, off = 12 + len;
	uint32_t crc = 0xFFFFFFFFu;
	const char* invalid = nullptr;
	auto feed = [&](const uint8_t* p, size_t n) { if (want_crc) crc = crc32c_step(crc, p, n, true); };
	// The CRC and the -32768 scan share one pass over each block so the bytes are read from memory once.
	auto consume = [&](const uint8_t* p, size_t n, bool scan) {
		constexpr size_t kBlock = 8192;
		bool bad = false;
		for (size_t at = 0; at < n; at += kBlock) {
			const size_t k = std::min(kBlock, n - at);
			feed(p + at, k);
			if (scan) bad |= any_min16(p + at, k / 2);
		}
		return bad;
	};
	auto take = [&](size_t n) -> uint8_t* {
		const size_t at = (cursor + 63) & ~size_t(63);
		if (at > small_.size() || n > small_.size() - at) { invalid = "internal buffer too small"; return nullptr; }
		cursor = at + n;
		return small_.data() + at;
	};
	feed(data, off);
	auto section = [&](unsigned s) {
		if (invalid) return;
		if (off > bytes_ || bytes_-off < 4 || u32(data+off) != hash+s) { invalid = "section hash mismatch"; return; }
		feed(data+off, 4);
		off += 4;
	};
	auto ints = [&](size_t n) -> const int16_t* {
		if (invalid) return nullptr;
		if (off > bytes_ || n > (bytes_-off)/2) { invalid = "truncated integer array"; return nullptr; }
		uint8_t* dst = take(n*2);
		if (!dst) return nullptr;
		const bool bad = consume(data+off, n*2, true);
		std::memcpy(dst, data+off, n*2);
		off += n*2;
		if (bad) { invalid = "forbidden -32768"; return nullptr; }
		return reinterpret_cast<const int16_t*>(dst);
	};
	auto pair_table = [&](size_t n) -> const uint8_t* {
		if (invalid) return nullptr;
		if (off > bytes_ || n > (bytes_-off)/2) { invalid = "truncated integer array"; return nullptr; }
		const uint8_t* src = data+off;
		const bool bad = consume(src, n*2, true);
		off += n*2;
		if (bad) { invalid = "forbidden -32768"; return nullptr; }
		return src;
	};
	auto floats = [&](size_t n, int mode) -> const float* {
		if (invalid) return nullptr;
		if (off > bytes_ || n > (bytes_-off)/4) { invalid = "truncated float array"; return nullptr; }
		uint8_t* dst = take(n*4);
		if (!dst) return nullptr;
		consume(data+off, n*4, false);
		std::memcpy(dst, data+off, n*4);
		off += n*4;
		const auto* p = reinterpret_cast<const float*>(dst);
		for (size_t i=0; i<n; ++i) {
			if (!std::isfinite(p[i]) || (mode && p[i] <= 0)) { invalid = "invalid float/scale"; return nullptr; }
			if (mode == 2) { int exp; if (std::frexp(p[i], &exp) != 0.5f) { invalid = "a1 is not a power of two"; return nullptr; } }
		}
		return p;
	};
	section(1); embed_ = ints(2344*128);
	section(2); pair_ = pair_table(2745996u*16);
	section(3); m_ = floats(400,0);
	section(4); w1_ = ints(512*800); inv1_ = floats(512,1); b1_ = floats(512,0);
	if (hidden2_) { section(5); a1_ = floats(512,2); w2_ = ints(256*512); inv2_ = floats(256,1); b2_ = floats(256,0); }
	section(6); vw_ = floats(hidden2_ ? 256 : 512,0); vb_ = floats(1,0);
	if (invalid) return reject(invalid);
	if (want_crc) {
		// A SYMq16c file must end with exactly the 8-byte trailer; a file cut short or extended is not accepted.
		if (bytes_ - off != kTrailerBytes || u32(data+off) != hash+7) return reject("missing or malformed checksum trailer");
		const uint32_t stored = u32(data+off+4), actual = ~crc;
		if (stored != actual) {
			char text[80];
			std::snprintf(text, sizeof(text), "checksum mismatch (stored %08x, computed %08x)", stored, actual);
			return reject(text);
		}
		checksum_verified_ = true;
	} else if (off != bytes_) {
		return reject("trailing data");
	}
	// The dense plans are built only for a file that passed every check above.
	if (!build_plan(w1_, 512, 800, plan1_)) return reject("cannot build the W1 plan");
	if (hidden2_ && !build_plan(w2_, 256, 512, plan2_)) return reject("cannot build the W2 plan");
	return true;
}
// ---------------------------------------------------------------------------------------------------------------
// Reference (scalar) accumulator code.
// ---------------------------------------------------------------------------------------------------------------
void Q16Network::token_delta(Q16Accumulator& a, int f, uint16_t t, int sign) const {
	for (int c=0;c<128;++c) {
		int64_t v = embed_[size_t(t)*128+c];
		a.p1[f][c] += sign*v; a.p2[f][c] += sign*v*v; a.p3[f][c] += sign*v*v*v;
	}
}
void Q16Network::pair_delta(Q16Accumulator& a, int f, uint16_t t, uint16_t u, int sign) const {
	int16_t p[16];
	std::memcpy(p, pair_ + size_t(pair_index(t,u))*32, sizeof(p)); // pair_ may start at an odd address (SYMq16c)
	for (int c=0;c<16;++c) a.pair[f][c] += sign*int32_t(p[c]);
}
bool Q16Network::rebuild_reference(const BoardView& b, Q16Accumulator& a, std::string* e) const {
	uint16_t ids[2][kMaxTokensPerFrame];
	if (!mapping_) return fail(e,"network not loaded");
	if (!get_tokens(b,ids,e)) return false;
	a = Q16Accumulator{}; a.network = this;
	for (int f=0; f<2; ++f) for (int i=0; i<40; ++i) {
		a.tokens[f][i] = ids[f][i]; token_delta(a,f,ids[f][i],1);
		for (int j=0;j<i;++j) pair_delta(a,f,ids[f][i],ids[f][j],1);
	}
	return true;
}
bool Q16Network::update_reference(const BoardView& b, Q16Accumulator& a, std::string* e) const {
	if (!mapping_ || a.network != this) return fail(e,"accumulator/network mismatch");
	uint16_t ids[2][kMaxTokensPerFrame];
	if (!get_tokens(b,ids,e)) return false;
	for (int f=0;f<2;++f) {
		std::array<uint16_t,40> current = a.tokens[f]; int n=40;
		for (int i=0;i<n;) {
			uint16_t t=current[i];
			if (std::binary_search(ids[f],ids[f]+40,t)) { ++i; continue; }
			current[i]=current[--n]; token_delta(a,f,t,-1);
			for (int j=0;j<n;++j) pair_delta(a,f,t,current[j],-1);
		}
		for (int i=0;i<40;++i) {
			uint16_t t=ids[f][i];
			if (std::find(current.begin(),current.begin()+n,t) != current.begin()+n) continue;
			for (int j=0;j<n;++j) pair_delta(a,f,t,current[j],1);
			token_delta(a,f,t,1); current[n++]=t;
		}
		std::copy(ids[f],ids[f]+40,a.tokens[f].begin());
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Token sets.
// ---------------------------------------------------------------------------------------------------------------
bool Q16Network::make_tokens(const BoardView& b, Q16Tokens& T, std::string* e) const {
	if (!validate_board(b, e)) return false;
	uint16_t raw[2][kMaxTokensPerFrame];
	if (collect_tokens(b, raw) != 40) return fail(e, "SYMq16 requires exactly 40 tokens");
	for (int f = 0; f < 2; ++f) {
		uint64_t* bits = T.bits[f];
		std::memset(bits, 0, sizeof(uint64_t) * kQ16TokenWords);
		for (int i = 0; i < 40; ++i) {
			const unsigned id = raw[f][i];
			if (id >= unsigned(kTokenSpace)) return fail(e, "invalid or duplicate token");
			const uint64_t bit = uint64_t(1) << (id & 63);
			if (bits[id >> 6] & bit) return fail(e, "invalid or duplicate token");
			bits[id >> 6] |= bit;
		}
		int n = 0;
		for (int w = 0; w < kQ16TokenWords; ++w) {
			uint64_t x = bits[w];
			while (x) { T.ids[f][n++] = uint16_t(w * 64 + __builtin_ctzll(x)); x &= x - 1; }
		}
	}
	return true;
}

#if defined(__AVX2__)
namespace {

// ---- exact int64 helpers for AVX2 -------------------------------------------------------------------------------
// Low 64 bits of a*b per lane (exact mod 2^64).
inline __m256i mullo_epi64(__m256i a, __m256i b) {
	const __m256i bswap = _mm256_shuffle_epi32(b, 0xB1);
	const __m256i cross = _mm256_mullo_epi32(a, bswap);
	__m256i sum = _mm256_add_epi32(cross, _mm256_srli_epi64(cross, 32));
	sum = _mm256_slli_epi64(sum, 32);
	return _mm256_add_epi64(_mm256_mul_epu32(a, b), sum);
}
// int64 -> double with the same rounding as cvtsi2sd (nearest even): hi*2^32 + lo, both parts exact, one rounding.
inline __m256d cvt_i64_pd(__m256i v) {
	const __m256i magic = _mm256_castpd_si256(_mm256_set1_pd(0x1.0p52));
	const __m256i lo = _mm256_and_si256(v, _mm256_set1_epi64x(0xFFFFFFFFLL));
	const __m256d dlo = _mm256_sub_pd(_mm256_castsi256_pd(_mm256_or_si256(lo, magic)), _mm256_set1_pd(0x1.0p52));
	const __m256i hi = _mm256_xor_si256(_mm256_srli_epi64(v, 32), _mm256_set1_epi64x(0x80000000LL));
	const __m256d dhi = _mm256_sub_pd(_mm256_castsi256_pd(_mm256_or_si256(hi, magic)),
	                                  _mm256_set1_pd(0x1.0p52 + 0x1.0p31));
	return _mm256_add_pd(_mm256_mul_pd(dhi, _mm256_set1_pd(0x1.0p32)), dlo);
}
// std::round (half away from zero), exact: t = trunc(y), the fraction y - t is exact.
inline __m256d round_half_away(__m256d y) {
	const __m256d t = _mm256_round_pd(y, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
	const __m256d frac = _mm256_andnot_pd(_mm256_set1_pd(-0.0), _mm256_sub_pd(y, t));
	const __m256d ge = _mm256_cmp_pd(frac, _mm256_set1_pd(0.5), _CMP_GE_OQ);
	const __m256d one = _mm256_or_pd(_mm256_set1_pd(1.0), _mm256_and_pd(y, _mm256_set1_pd(-0.0)));
	return _mm256_add_pd(t, _mm256_and_pd(ge, one));
}
// x -> int16 like quant(): round, clamp to +-32767, count clamps. 8 doubles in, 8 int16 out.
inline void quant8(const double* x, double r, int16_t* out, uint32_t& clamps) {
	const __m256d vr = _mm256_set1_pd(r), hi = _mm256_set1_pd(32767.0), lo = _mm256_set1_pd(-32767.0);
	__m256d t0 = round_half_away(_mm256_mul_pd(_mm256_load_pd(x), vr));
	__m256d t1 = round_half_away(_mm256_mul_pd(_mm256_load_pd(x + 4), vr));
	clamps += uint32_t(__builtin_popcount(_mm256_movemask_pd(_mm256_or_pd(_mm256_cmp_pd(t0, hi, _CMP_GT_OQ), _mm256_cmp_pd(t0, lo, _CMP_LT_OQ)))));
	clamps += uint32_t(__builtin_popcount(_mm256_movemask_pd(_mm256_or_pd(_mm256_cmp_pd(t1, hi, _CMP_GT_OQ), _mm256_cmp_pd(t1, lo, _CMP_LT_OQ)))));
	t0 = _mm256_min_pd(_mm256_max_pd(t0, lo), hi);
	t1 = _mm256_min_pd(_mm256_max_pd(t1, lo), hi);
	const __m128i a = _mm256_cvttpd_epi32(t0), b = _mm256_cvttpd_epi32(t1);
	_mm_store_si128(reinterpret_cast<__m128i*>(out), _mm_packs_epi32(a, b));
}

// ---- power sums ----------------------------------------------------------------------------------------------------
struct PowAcc { __m256i p1a, p1b, p2a, p2b, p3a, p3b; };
inline PowAcc pow_load(const Q16Accumulator& a, int f, int c) {
	PowAcc r;
	r.p1a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p1[f][c])); r.p1b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p1[f][c+4]));
	r.p2a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p2[f][c])); r.p2b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p2[f][c+4]));
	r.p3a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p3[f][c])); r.p3b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p3[f][c+4]));
	return r;
}
inline void pow_store(Q16Accumulator& a, int f, int c, const PowAcc& r) {
	_mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.p1[f][c]), r.p1a); _mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.p1[f][c+4]), r.p1b);
	_mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.p2[f][c]), r.p2a); _mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.p2[f][c+4]), r.p2b);
	_mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.p3[f][c]), r.p3a); _mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.p3[f][c+4]), r.p3b);
}
// acc += (Sub ? -1 : 1) * (v, v^2, v^3) for the 8 channels at e8.
template<bool Sub>
inline void pow_add(PowAcc& A, const int16_t* e8) {
	const __m256i v = _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(e8)));
	const __m256i v2 = _mm256_mullo_epi32(v, v); // <= 32767^2 < 2^30
	const __m256i v1a = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v)), v1b = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1));
	const __m256i v2a = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v2)), v2b = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v2, 1));
	const __m256i ev = _mm256_mul_epi32(v2, v);
	const __m256i od = _mm256_mul_epi32(_mm256_srli_epi64(v2, 32), _mm256_srli_epi64(v, 32));
	const __m256i ul = _mm256_unpacklo_epi64(ev, od), uh = _mm256_unpackhi_epi64(ev, od);
	const __m256i v3a = _mm256_permute2x128_si256(ul, uh, 0x20), v3b = _mm256_permute2x128_si256(ul, uh, 0x31);
	if (Sub) {
		A.p1a = _mm256_sub_epi64(A.p1a, v1a); A.p1b = _mm256_sub_epi64(A.p1b, v1b);
		A.p2a = _mm256_sub_epi64(A.p2a, v2a); A.p2b = _mm256_sub_epi64(A.p2b, v2b);
		A.p3a = _mm256_sub_epi64(A.p3a, v3a); A.p3b = _mm256_sub_epi64(A.p3b, v3b);
	} else {
		A.p1a = _mm256_add_epi64(A.p1a, v1a); A.p1b = _mm256_add_epi64(A.p1b, v1b);
		A.p2a = _mm256_add_epi64(A.p2a, v2a); A.p2b = _mm256_add_epi64(A.p2b, v2b);
		A.p3a = _mm256_add_epi64(A.p3a, v3a); A.p3b = _mm256_add_epi64(A.p3b, v3b);
	}
}
// 16 int32 channel sums (two vectors) += or -= a row of 16 int16.
struct PairAcc { __m256i lo, hi; };
inline void pair_add(PairAcc& s, const uint8_t* row) { // 32 bytes at any address
	s.lo = _mm256_add_epi32(s.lo, _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row))));
	s.hi = _mm256_add_epi32(s.hi, _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + 16))));
}
// Sum of the pair rows listed in idx[0..n). (Software prefetch of the rows was measured: no difference.)
inline PairAcc pair_sum(const uint8_t* pair, const uint32_t* idx, int n) {
	PairAcc s{_mm256_setzero_si256(), _mm256_setzero_si256()};
	for (int i = 0; i < n; ++i) pair_add(s, pair + size_t(idx[i]) * 32);
	return s;
}
} // namespace

bool Q16Network::rebuild(const Q16Tokens& T, Q16Accumulator& a) const {
	if (!mapping_) return false;
	for (int f = 0; f < 2; ++f) {
		const uint16_t* ids = T.ids[f];
		for (int c = 0; c < 128; c += 8) {
			PowAcc A{_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256(),
			         _mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};
			for (int i = 0; i < 40; ++i) pow_add<false>(A, embed_ + size_t(ids[i]) * 128 + c);
			pow_store(a, f, c, A);
		}
		// All 780 pairs. ids is sorted ascending, so for a fixed j every pair (ids[j], ids[i]), i > j has low = ids[j].
		uint32_t idx[780]; int n = 0;
		for (int j = 0; j < 39; ++j) {
			const uint32_t low = ids[j];
			const uint32_t base = low * (2u * kTokenSpace - low - 1u) / 2u - low - 1u;
			for (int i = j + 1; i < 40; ++i) idx[n++] = base + ids[i];
		}
		const PairAcc s = pair_sum(pair_, idx, n);
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.pair[f][0]), s.lo);
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(&a.pair[f][8]), s.hi);
		for (int i = 0; i < 40; ++i) a.tokens[f][i] = ids[i];
	}
	a.network = this;
	return true;
}

bool Q16Network::update_from(const Q16Tokens& T, const Q16Accumulator& base, Q16Accumulator& out, std::string* e) const {
	if (!mapping_ || base.network != this) return fail(e, "accumulator/network mismatch");
	for (int f = 0; f < 2; ++f) {
		for (int i = 0; i < 40; ++i)
			if (base.tokens[f][i] >= kTokenSpace) return fail(e, "invalid token in the base accumulator");
	}
	for (int f = 0; f < 2; ++f) {
		uint64_t old_bits[kQ16TokenWords];
		q16_token_bits(base.tokens[f].data(), old_bits);
		const uint64_t* new_bits = T.bits[f];
		uint16_t A[40], R[40], K[40]; int na = 0, nr = 0, nk = 0;
		for (int w = 0; w < kQ16TokenWords; ++w) {
			uint64_t add = new_bits[w] & ~old_bits[w], rem = old_bits[w] & ~new_bits[w], keep = new_bits[w] & old_bits[w];
			while (add) { A[na++] = uint16_t(w * 64 + __builtin_ctzll(add)); add &= add - 1; }
			while (rem) { R[nr++] = uint16_t(w * 64 + __builtin_ctzll(rem)); rem &= rem - 1; }
			while (keep) { K[nk++] = uint16_t(w * 64 + __builtin_ctzll(keep)); keep &= keep - 1; }
		}
		// Both sets hold 40 tokens, so na == nr == 40 - nk.
		for (int c = 0; c < 128; c += 8) {
			PowAcc P = pow_load(base, f, c);
			for (int i = 0; i < nr; ++i) pow_add<true>(P, embed_ + size_t(R[i]) * 128 + c);
			for (int i = 0; i < na; ++i) pow_add<false>(P, embed_ + size_t(A[i]) * 128 + c);
			pow_store(out, f, c, P);
		}
		// pairs(new) = pairs(K) + A x K + pairs(A);  pairs(old) = pairs(K) + R x K + pairs(R).
		uint32_t plus[800], minus[800]; int np = 0, nm = 0;
		for (int i = 0; i < na; ++i) {
			for (int k = 0; k < nk; ++k) plus[np++] = pair_index(A[i], K[k]);
			for (int j = 0; j < i; ++j) plus[np++] = pair_index(A[i], A[j]);
		}
		for (int i = 0; i < nr; ++i) {
			for (int k = 0; k < nk; ++k) minus[nm++] = pair_index(R[i], K[k]);
			for (int j = 0; j < i; ++j) minus[nm++] = pair_index(R[i], R[j]);
		}
		const PairAcc sp = pair_sum(pair_, plus, np), sm = pair_sum(pair_, minus, nm);
		const __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&base.pair[f][0]));
		const __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&base.pair[f][8]));
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(&out.pair[f][0]), _mm256_sub_epi32(_mm256_add_epi32(b0, sp.lo), sm.lo));
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(&out.pair[f][8]), _mm256_sub_epi32(_mm256_add_epi32(b1, sp.hi), sm.hi));
		for (int i = 0; i < 40; ++i) out.tokens[f][i] = T.ids[f][i];
	}
	out.network = this;
	return true;
}

#else // !__AVX2__ : scalar versions of the token-set entry points

bool Q16Network::rebuild(const Q16Tokens& T, Q16Accumulator& a) const {
	if (!mapping_) return false;
	a = Q16Accumulator{}; a.network = this;
	for (int f = 0; f < 2; ++f) for (int i = 0; i < 40; ++i) {
		a.tokens[f][i] = T.ids[f][i]; token_delta(a, f, T.ids[f][i], 1);
		for (int j = 0; j < i; ++j) pair_delta(a, f, T.ids[f][i], T.ids[f][j], 1);
	}
	return true;
}
bool Q16Network::update_from(const Q16Tokens& T, const Q16Accumulator& base, Q16Accumulator& out, std::string* e) const {
	if (!mapping_ || base.network != this) return fail(e, "accumulator/network mismatch");
	for (int f = 0; f < 2; ++f)
		for (int i = 0; i < 40; ++i)
			if (base.tokens[f][i] >= kTokenSpace) return fail(e, "invalid token in the base accumulator");
	Q16Accumulator a = base;
	for (int f = 0; f < 2; ++f) {
		uint64_t old_bits[kQ16TokenWords];
		q16_token_bits(base.tokens[f].data(), old_bits);
		uint16_t A[40], R[40], K[40]; int na = 0, nr = 0, nk = 0;
		for (int w = 0; w < kQ16TokenWords; ++w) {
			uint64_t add = T.bits[f][w] & ~old_bits[w], rem = old_bits[w] & ~T.bits[f][w], keep = T.bits[f][w] & old_bits[w];
			while (add) { A[na++] = uint16_t(w * 64 + __builtin_ctzll(add)); add &= add - 1; }
			while (rem) { R[nr++] = uint16_t(w * 64 + __builtin_ctzll(rem)); rem &= rem - 1; }
			while (keep) { K[nk++] = uint16_t(w * 64 + __builtin_ctzll(keep)); keep &= keep - 1; }
		}
		for (int i = 0; i < nr; ++i) {
			token_delta(a, f, R[i], -1);
			for (int k = 0; k < nk; ++k) pair_delta(a, f, R[i], K[k], -1);
			for (int j = 0; j < i; ++j) pair_delta(a, f, R[i], R[j], -1);
		}
		for (int i = 0; i < na; ++i) {
			token_delta(a, f, A[i], 1);
			for (int k = 0; k < nk; ++k) pair_delta(a, f, A[i], K[k], 1);
			for (int j = 0; j < i; ++j) pair_delta(a, f, A[i], A[j], 1);
		}
		for (int i = 0; i < 40; ++i) a.tokens[f][i] = T.ids[f][i];
	}
	a.network = this;
	out = a;
	return true;
}
#endif

bool Q16Network::rebuild(const BoardView& b, Q16Accumulator& a, std::string* e) const {
	if (!mapping_) return fail(e, "network not loaded");
	Q16Tokens T;
	if (!make_tokens(b, T, e)) return false;
	return rebuild(T, a);
}
bool Q16Network::update(const BoardView& b, Q16Accumulator& a, std::string* e) const {
	if (!mapping_ || a.network != this) return fail(e, "accumulator/network mismatch");
	Q16Tokens T;
	if (!make_tokens(b, T, e)) return false;
	return update_from(T, a, a, e);
}

// ---------------------------------------------------------------------------------------------------------------
// Evaluation.
// ---------------------------------------------------------------------------------------------------------------
bool Q16Network::evaluate_reference(const Q16Accumulator& a, unsigned stm, Q16Result& out, bool simd, std::string* e) const {
	if (!mapping_ || a.network != this || stm>1) return fail(e,"invalid evaluation state");
	{
		out = Q16Result{};
		double x[800], maximum=0;
		for (int slot=0;slot<2;++slot) {
			int f=slot ? 1-int(stm) : int(stm);
			for (int k=0;k<4;++k) {
				double block[128], ss=0; int n=k==3?16:128;
				for (int c=0;c<n;++c) {
					int64_t p=a.p1[f][c], v;
					if (k==0) v=p;
					else if (k==1) v=p*p-a.p2[f][c];
					else if (k==2) v=p*p*p-3*p*a.p2[f][c]+2*a.p3[f][c];
					else v=a.pair[f][c];
					block[c]=double(v); ss+=block[c]*block[c];
				}
				double rms=std::sqrt(ss/n);
				for (int c=0;c<n;++c) {
					int column=k*128+c, i=slot*400+column;
					x[i]=rms==0?0:block[c]/rms*double(m_[column]); maximum=std::max(maximum,std::abs(x[i]));
				}
			}
		}
		out.r=maximum>32767?32767/maximum:1;
		for (int i=0;i<800;++i) if (!quant(x[i]*out.r,out.clamps,out.x[i])) return fail(e,"non-finite quantization");
		float head1[512], head2[256];
		for (int i=0;i<512;++i) {
			out.acc1[i]=q16_dot(out.x.data(),w1_+i*800,800,simd);
			float mul=inv1_[i]/float(out.r);
			float z=float(out.acc1[i])*mul; z=z+b1_[i];
			if (!std::isfinite(z)) return fail(e,"non-finite dense output");
			head1[i]=std::max(z,0.0f);
		}
		if (hidden2_) {
			double h[512], maximum2=0;
			for (int i=0;i<512;++i) { h[i]=double(head1[i])*double(a1_[i]); maximum2=std::max(maximum2,h[i]); }
			out.r2=maximum2>32767?32767/maximum2:1;
			for (int i=0;i<512;++i) if (!quant(h[i]*out.r2,out.clamps,out.hidden[i])) return fail(e,"non-finite quantization");
			for (int i=0;i<256;++i) {
				out.acc2[i]=q16_dot(out.hidden.data(),w2_+i*512,512,simd);
				float mul=inv2_[i]/float(out.r2); float z=float(out.acc2[i])*mul; z=z+b2_[i];
				if (!std::isfinite(z)) return fail(e,"non-finite dense output");
				head2[i]=std::max(z,0.0f);
			}
		}
		float lanes[8]{}; const float* head=hidden2_?head2:head1;
		for (int i=0;i<(hidden2_?256:512);++i) lanes[i%8]+=head[i]*vw_[i];
		float pre=*vb_; for (float v:lanes) pre+=v;
		if (!std::isfinite(pre)) return fail(e,"non-finite value");
		out.pre=pre; out.value=float(std::tanh(double(pre)));
		out.cp=static_cast<int32_t>(std::max(-4.951718775643098,std::min(4.951718775643098,double(pre)))*1512.173);
		return true;
	}
}

#if defined(__AVX2__)
namespace {
// Rows computed with one i32 accumulation of N vpmaddwd results per widening (see build_plan).
inline void widen_add(__m256i& lo, __m256i& hi, __m256i p) {
	lo = _mm256_add_epi64(lo, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(p)));
	hi = _mm256_add_epi64(hi, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(p, 1)));
}
template<int N>
inline void dense_group(const int16_t* w, int iters, const int16_t* x, int64_t* out4) {
	const size_t stride = size_t(iters) * 16;
	const int16_t *w0 = w, *w1 = w + stride, *w2 = w + 2 * stride, *w3 = w + 3 * stride;
	__m256i lo0 = _mm256_setzero_si256(), hi0 = lo0, lo1 = lo0, hi1 = lo0, lo2 = lo0, hi2 = lo0, lo3 = lo0, hi3 = lo0;
	int it = 0;
	for (; it + N <= iters; it += N) {
		__m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;
#pragma GCC unroll 8
		for (int k = 0; k < N; ++k) {
			const size_t o = size_t(it + k) * 16;
			const __m256i xv = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + o));
			a0 = _mm256_add_epi32(a0, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w0 + o)), xv));
			a1 = _mm256_add_epi32(a1, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w1 + o)), xv));
			a2 = _mm256_add_epi32(a2, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w2 + o)), xv));
			a3 = _mm256_add_epi32(a3, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w3 + o)), xv));
			// Keep the four additions as sequential chains. Without this GCC re-associates the sums into trees,
			// computes all the vpmaddwd results first and spills them to the stack (measured 4.7 vs 2.9 cycles per step).
			asm("" : "+x"(a0), "+x"(a1), "+x"(a2), "+x"(a3));
		}
		widen_add(lo0, hi0, a0); widen_add(lo1, hi1, a1); widen_add(lo2, hi2, a2); widen_add(lo3, hi3, a3);
	}
	if (it < iters) { // the last, shorter chunk
		__m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;
		for (; it < iters; ++it) {
			const size_t o = size_t(it) * 16;
			const __m256i xv = _mm256_load_si256(reinterpret_cast<const __m256i*>(x + o));
			a0 = _mm256_add_epi32(a0, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w0 + o)), xv));
			a1 = _mm256_add_epi32(a1, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w1 + o)), xv));
			a2 = _mm256_add_epi32(a2, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w2 + o)), xv));
			a3 = _mm256_add_epi32(a3, _mm256_madd_epi16(_mm256_load_si256(reinterpret_cast<const __m256i*>(w3 + o)), xv));
			asm("" : "+x"(a0), "+x"(a1), "+x"(a2), "+x"(a3));
		}
		widen_add(lo0, hi0, a0); widen_add(lo1, hi1, a1); widen_add(lo2, hi2, a2); widen_add(lo3, hi3, a3);
	}
	// horizontal sums of the four rows (each row: lo + hi, four i64 lanes)
	const __m256i v0 = _mm256_add_epi64(lo0, hi0), v1 = _mm256_add_epi64(lo1, hi1);
	const __m256i v2 = _mm256_add_epi64(lo2, hi2), v3 = _mm256_add_epi64(lo3, hi3);
	const __m256i t0 = _mm256_add_epi64(_mm256_unpacklo_epi64(v0, v1), _mm256_unpackhi_epi64(v0, v1));
	const __m256i t1 = _mm256_add_epi64(_mm256_unpacklo_epi64(v2, v3), _mm256_unpackhi_epi64(v2, v3));
	const __m256i s = _mm256_add_epi64(_mm256_permute2x128_si256(t0, t1, 0x20), _mm256_permute2x128_si256(t0, t1, 0x31));
	_mm256_storeu_si256(reinterpret_cast<__m256i*>(out4), s);
}
template<int N>
inline void dense_run(const int16_t* w, int iters, const int16_t* x, const uint16_t* row_of_slot, int groups, int64_t* out) {
	for (int g = 0; g < groups; ++g) {
		int64_t r[4];
		dense_group<N>(w + size_t(g) * 4 * iters * 16, iters, x, r);
		out[row_of_slot[g*4]] = r[0]; out[row_of_slot[g*4+1]] = r[1]; out[row_of_slot[g*4+2]] = r[2]; out[row_of_slot[g*4+3]] = r[3];
	}
}
} // namespace
#endif

bool Q16Network::evaluate_fast(const Q16Accumulator& a, unsigned stm, Q16Result* res, float* pre_out, std::string* e) const {
#if defined(__AVX2__)
	if (!mapping_ || a.network != this || stm > 1) return fail(e, "invalid evaluation state");
	alignas(32) double blk[2][3][128];
	alignas(32) double pr[2][16];
	alignas(32) double xd[800];
	alignas(32) int16_t xi[800];
	alignas(32) int64_t acc1[512];
	alignas(32) float head1[512];
	alignas(32) int16_t hid[512];
	alignas(32) int64_t acc2[256];
	alignas(32) float head2[256];
	const auto bail = [&]() { // anything unusual: let the scalar reference produce the answer (or the error)
		Q16Result tmp;
		if (!evaluate_reference(a, stm, tmp, true, e)) return false;
		if (pre_out) *pre_out = tmp.pre;
		if (res) *res = tmp;
		return true;
	};

	// --- Newton blocks -> doubles
	for (int slot = 0; slot < 2; ++slot) {
		const int f = slot ? 1 - int(stm) : int(stm);
		for (int c = 0; c < 128; c += 4) {
			const __m256i p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p1[f][c]));
			const __m256i q2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p2[f][c]));
			const __m256i q3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&a.p3[f][c]));
			const __m256i pp = _mm256_mul_epi32(p, p);                // |p1| <= 40*32767 < 2^31
			const __m256i v1 = _mm256_sub_epi64(pp, q2);
			const __m256i ppp = mullo_epi64(pp, p);
			const __m256i p3x = _mm256_add_epi64(_mm256_add_epi64(p, p), p);
			const __m256i v2 = _mm256_add_epi64(_mm256_sub_epi64(ppp, mullo_epi64(p3x, q2)), _mm256_add_epi64(q3, q3));
			_mm256_store_pd(&blk[slot][0][c], cvt_i64_pd(p));
			_mm256_store_pd(&blk[slot][1][c], cvt_i64_pd(v1));
			_mm256_store_pd(&blk[slot][2][c], cvt_i64_pd(v2));
		}
		for (int c = 0; c < 16; c += 4)
			_mm256_store_pd(&pr[slot][c], _mm256_cvtepi32_pd(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&a.pair[f][c]))));
	}
	// --- sums of squares in the reference order (c ascending), the eight independent chains interleaved
	double ss00 = 0, ss01 = 0, ss02 = 0, ss10 = 0, ss11 = 0, ss12 = 0, ss03 = 0, ss13 = 0;
	for (int c = 0; c < 128; ++c) {
		ss00 += blk[0][0][c] * blk[0][0][c]; ss01 += blk[0][1][c] * blk[0][1][c]; ss02 += blk[0][2][c] * blk[0][2][c];
		ss10 += blk[1][0][c] * blk[1][0][c]; ss11 += blk[1][1][c] * blk[1][1][c]; ss12 += blk[1][2][c] * blk[1][2][c];
	}
	for (int c = 0; c < 16; ++c) { ss03 += pr[0][c] * pr[0][c]; ss13 += pr[1][c] * pr[1][c]; }
	const double rms[2][4] = {
		{std::sqrt(ss00 / 128), std::sqrt(ss01 / 128), std::sqrt(ss02 / 128), std::sqrt(ss03 / 16)},
		{std::sqrt(ss10 / 128), std::sqrt(ss11 / 128), std::sqrt(ss12 / 128), std::sqrt(ss13 / 16)}};
	// --- x = block / rms * m  (zero when rms == 0), and max |x|
	__m256d vmax = _mm256_setzero_pd();
	__m256d bad = _mm256_setzero_pd();
	const __m256d absmask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
	for (int slot = 0; slot < 2; ++slot) {
		for (int k = 0; k < 4; ++k) {
			const int n = k == 3 ? 16 : 128;
			const double* src = k == 3 ? pr[slot] : blk[slot][k];
			double* dst = xd + slot * 400 + k * 128;
			const double rm = rms[slot][k];
			if (rm == 0) { for (int c = 0; c < n; ++c) dst[c] = 0; continue; }
			const __m256d vr = _mm256_set1_pd(rm);
			for (int c = 0; c < n; c += 4) {
				const __m256d m = _mm256_cvtps_pd(_mm_loadu_ps(m_ + k * 128 + c));
				const __m256d x = _mm256_mul_pd(_mm256_div_pd(_mm256_load_pd(src + c), vr), m);
				_mm256_store_pd(dst + c, x);
				vmax = _mm256_max_pd(vmax, _mm256_and_pd(x, absmask));
				bad = _mm256_or_pd(bad, _mm256_cmp_pd(x, x, _CMP_UNORD_Q));
			}
		}
	}
	alignas(32) double mv[4]; _mm256_store_pd(mv, vmax);
	const double maximum = std::max(std::max(mv[0], mv[1]), std::max(mv[2], mv[3]));
	if (_mm256_movemask_pd(bad) || !(maximum <= DBL_MAX)) return bail();
	const double r = maximum > 32767 ? 32767 / maximum : 1;
	uint32_t clamps = 0;
	for (int i = 0; i < 800; i += 8) quant8(xd + i, r, xi + i, clamps);

	// --- W1
	{
		const DensePlan& p = plan1_;
		const int iters = p.cols / 16;
		const int16_t* w = p.packed.data(); const uint16_t* rs = p.row_of_slot.data(); int slot = 0;
		for (const auto& run : p.runs) {
			switch (run.chunk) {
			case 8: dense_run<8>(w + size_t(slot) * p.cols, iters, xi, rs + slot, run.groups, acc1); break;
			case 6: dense_run<6>(w + size_t(slot) * p.cols, iters, xi, rs + slot, run.groups, acc1); break;
			case 4: dense_run<4>(w + size_t(slot) * p.cols, iters, xi, rs + slot, run.groups, acc1); break;
			case 3: dense_run<3>(w + size_t(slot) * p.cols, iters, xi, rs + slot, run.groups, acc1); break;
			case 2: dense_run<2>(w + size_t(slot) * p.cols, iters, xi, rs + slot, run.groups, acc1); break;
			default: dense_run<1>(w + size_t(slot) * p.cols, iters, xi, rs + slot, run.groups, acc1); break;
			}
			slot += run.groups * 4;
		}
		for (int t = 0; t < p.tail_rows; ++t, ++slot)
			acc1[rs[slot]] = q16_dot(xi, w + size_t(slot) * p.cols, p.cols, true);
	}
	// --- W1 post: z = float(acc) * (inv / float(r)) + b ; h = max(z, 0)
	{
		const __m256 rf = _mm256_set1_ps(float(r)), zero = _mm256_setzero_ps();
		__m256 badf = _mm256_setzero_ps();
		for (int i = 0; i < 512; i += 8) {
			const __m128 fa = _mm256_cvtpd_ps(cvt_i64_pd(_mm256_load_si256(reinterpret_cast<const __m256i*>(acc1 + i))));
			const __m128 fb = _mm256_cvtpd_ps(cvt_i64_pd(_mm256_load_si256(reinterpret_cast<const __m256i*>(acc1 + i + 4))));
			const __m256 accf = _mm256_insertf128_ps(_mm256_castps128_ps256(fa), fb, 1);
			const __m256 mul = _mm256_div_ps(_mm256_loadu_ps(inv1_ + i), rf);
			__m256 z = _mm256_mul_ps(accf, mul);
			z = _mm256_add_ps(z, _mm256_loadu_ps(b1_ + i));
			const __m256 d = _mm256_sub_ps(z, z);
			badf = _mm256_or_ps(badf, _mm256_cmp_ps(d, d, _CMP_UNORD_Q));
			_mm256_store_ps(head1 + i, _mm256_max_ps(zero, z));
		}
		if (_mm256_movemask_ps(badf)) return bail();
	}
	double r2 = 1;
	const float* head = head1;
	if (hidden2_) {
		alignas(32) double hd[512];
		__m256d hmax = _mm256_setzero_pd();
		for (int i = 0; i < 512; i += 4) {
			const __m256d h = _mm256_mul_pd(_mm256_cvtps_pd(_mm_load_ps(head1 + i)), _mm256_cvtps_pd(_mm_loadu_ps(a1_ + i)));
			_mm256_store_pd(hd + i, h);
			hmax = _mm256_max_pd(hmax, h);
		}
		_mm256_store_pd(mv, hmax);
		const double maximum2 = std::max(std::max(mv[0], mv[1]), std::max(mv[2], mv[3]));
		if (!(maximum2 <= DBL_MAX)) return bail();
		r2 = maximum2 > 32767 ? 32767 / maximum2 : 1;
		for (int i = 0; i < 512; i += 8) quant8(hd + i, r2, hid + i, clamps);
		{
			const DensePlan& p = plan2_;
			const int iters = p.cols / 16;
			const int16_t* w = p.packed.data(); const uint16_t* rs = p.row_of_slot.data(); int slot = 0;
			for (const auto& run : p.runs) {
				switch (run.chunk) {
				case 8: dense_run<8>(w + size_t(slot) * p.cols, iters, hid, rs + slot, run.groups, acc2); break;
				case 6: dense_run<6>(w + size_t(slot) * p.cols, iters, hid, rs + slot, run.groups, acc2); break;
				case 4: dense_run<4>(w + size_t(slot) * p.cols, iters, hid, rs + slot, run.groups, acc2); break;
				case 3: dense_run<3>(w + size_t(slot) * p.cols, iters, hid, rs + slot, run.groups, acc2); break;
				case 2: dense_run<2>(w + size_t(slot) * p.cols, iters, hid, rs + slot, run.groups, acc2); break;
				default: dense_run<1>(w + size_t(slot) * p.cols, iters, hid, rs + slot, run.groups, acc2); break;
				}
				slot += run.groups * 4;
			}
			for (int t = 0; t < p.tail_rows; ++t, ++slot)
				acc2[rs[slot]] = q16_dot(hid, w + size_t(slot) * p.cols, p.cols, true);
		}
		const __m256 rf = _mm256_set1_ps(float(r2)), zero = _mm256_setzero_ps();
		__m256 badf = _mm256_setzero_ps();
		for (int i = 0; i < 256; i += 8) {
			const __m128 fa = _mm256_cvtpd_ps(cvt_i64_pd(_mm256_load_si256(reinterpret_cast<const __m256i*>(acc2 + i))));
			const __m128 fb = _mm256_cvtpd_ps(cvt_i64_pd(_mm256_load_si256(reinterpret_cast<const __m256i*>(acc2 + i + 4))));
			const __m256 accf = _mm256_insertf128_ps(_mm256_castps128_ps256(fa), fb, 1);
			const __m256 mul = _mm256_div_ps(_mm256_loadu_ps(inv2_ + i), rf);
			__m256 z = _mm256_mul_ps(accf, mul);
			z = _mm256_add_ps(z, _mm256_loadu_ps(b2_ + i));
			const __m256 d = _mm256_sub_ps(z, z);
			badf = _mm256_or_ps(badf, _mm256_cmp_ps(d, d, _CMP_UNORD_Q));
			_mm256_store_ps(head2 + i, _mm256_max_ps(zero, z));
		}
		if (_mm256_movemask_ps(badf)) return bail();
		head = head2;
	}
	// --- value row: eight f32 lanes (i % 8), then the lanes in order
	{
		__m256 lanes = _mm256_setzero_ps();
		const int n = hidden2_ ? 256 : 512;
		for (int i = 0; i < n; i += 8)
			lanes = _mm256_add_ps(lanes, _mm256_mul_ps(_mm256_load_ps(head + i), _mm256_loadu_ps(vw_ + i)));
		alignas(32) float l[8]; _mm256_store_ps(l, lanes);
		float pre = *vb_; for (float v : l) pre += v;
		if (!std::isfinite(pre)) return bail();
		if (pre_out) *pre_out = pre;
		if (res) {
			*res = Q16Result{};
			res->pre = pre; res->value = float(std::tanh(double(pre)));
			res->cp = static_cast<int32_t>(std::max(-4.951718775643098, std::min(4.951718775643098, double(pre))) * 1512.173);
			res->r = r; res->r2 = r2; res->clamps = clamps;
			std::memcpy(res->x.data(), xi, sizeof xi);
			std::memcpy(res->acc1.data(), acc1, sizeof acc1);
			if (hidden2_) {
				std::memcpy(res->hidden.data(), hid, sizeof hid);
				std::memcpy(res->acc2.data(), acc2, sizeof acc2);
			}
		}
	}
	return true;
#else
	(void)a; (void)stm; (void)res; (void)pre_out;
	return fail(e, "no SIMD path");
#endif
}

bool Q16Network::evaluate(const Q16Accumulator& a, unsigned stm, Q16Result& out, bool simd, std::string* e) const {
#if defined(__AVX2__)
	if (simd) return evaluate_fast(a, stm, &out, nullptr, e);
#endif
	return evaluate_reference(a, stm, out, simd, e);
}
bool Q16Network::evaluate_pre(const Q16Accumulator& a, unsigned stm, float* pre, std::string* e) const {
#if defined(__AVX2__)
	return evaluate_fast(a, stm, nullptr, pre, e);
#else
	Q16Result r;
	if (!evaluate_reference(a, stm, r, true, e)) return false;
	*pre = r.pre;
	return true;
#endif
}
} } }
