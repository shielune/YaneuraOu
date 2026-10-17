#ifndef MANAKA_CORE_Q16_H_INCLUDED
#define MANAKA_CORE_Q16_H_INCLUDED

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "board.h"

namespace YaneuraOu { namespace Eval { namespace ManakaCore {

class Q16Network;
// Inspect/copy this state for undo, but only rebuild/update may modify its fields.
struct Q16Accumulator {
	std::array<std::array<int64_t, 128>, 2> p1{}, p2{}, p3{};
	std::array<std::array<int32_t, 16>, 2> pair{};
	std::array<std::array<uint16_t, 40>, 2> tokens{};
	const Q16Network* network = nullptr;
};
struct Q16Result {
	float pre = 0, value = 0;
	// Network cp: trunc(clamp(pre, +/-atanh(.9999))*1512.173). The engine-internal 0.9 scale is applied by the glue.
	int32_t cp = 0;
	double r = 1, r2 = 1;
	uint32_t clamps = 0;
	std::array<int16_t, 800> x{};
	std::array<int16_t, 512> hidden{};
	std::array<int64_t, 512> acc1{};
	std::array<int64_t, 256> acc2{};
};

// The 40 tokens of a position in both frames, as sorted id lists and as 2344-bit sets.
// Built once per position (make_tokens) and then used to pick a base accumulator and to update it.
constexpr int kQ16TokenWords = 37; // ceil(2344 / 64)
struct Q16Tokens {
	alignas(32) uint16_t ids[2][40];
	uint64_t bits[2][kQ16TokenWords];
};
// Number of tokens in a that are not in b (both sets have 40 tokens, so it is also the number in b that are
// not in a). a and b are bit sets of kQ16TokenWords words.
int q16_token_diff(const uint64_t* a, const uint64_t* b);
// Bit set of 40 token ids (any order).
void q16_token_bits(const uint16_t* ids, uint64_t* bits);

// Inputs must be in [-32767,32767], n >= 0. All loaded weights/features obey this.
// Exact i64 dot; SIMD widens every vpmaddwd result before accumulation.
int64_t q16_dot(const int16_t* a, const int16_t* b, int n, bool simd);
class Q16Network {
public:
	Q16Network() = default;
	~Q16Network();
	Q16Network(const Q16Network&) = delete;
	Q16Network& operator=(const Q16Network&) = delete;
	// ModelType=SYMq16c files carry a CRC32C trailer that is always verified; ModelType=SYMq16 files have none
	// and are accepted unverified unless require_checksum is set. checksum_verified() tells which happened.
	bool load(const std::string& path, std::string* error, bool require_checksum = false);

	// BoardView entry points (kept from the first version). rebuild builds from scratch, update changes an
	// accumulator that was built by the same network in place. Both use the SIMD path where it exists.
	bool rebuild(const BoardView&, Q16Accumulator&, std::string* error) const;
	bool update(const BoardView&, Q16Accumulator&, std::string* error) const;
	// The scalar first version of rebuild/update, kept as the reference the fast paths are checked against.
	bool rebuild_reference(const BoardView&, Q16Accumulator&, std::string* error) const;
	bool update_reference(const BoardView&, Q16Accumulator&, std::string* error) const;

	// Token-set entry points. out = base + (difference of token sets); base and out may be the same object.
	// The result is bit-identical to rebuild: the sums are exact integers.
	bool make_tokens(const BoardView&, Q16Tokens&, std::string* error) const;
	bool rebuild(const Q16Tokens&, Q16Accumulator&) const;
	bool update_from(const Q16Tokens&, const Q16Accumulator& base, Q16Accumulator& out, std::string* error) const;

	// simd = false is the scalar reference; simd = true is the fast path (the same values, bit for bit).
	bool evaluate(const Q16Accumulator&, unsigned side_to_move, Q16Result&, bool simd = true,
	              std::string* error = nullptr) const;
	// Fast path that returns only pre (the f32 before tanh) and does not fill a Q16Result.
	bool evaluate_pre(const Q16Accumulator&, unsigned side_to_move, float* pre, std::string* error = nullptr) const;
	bool has_hidden2() const { return hidden2_; }
	bool checksum_verified() const { return checksum_verified_; }
private:
	void reset();
	void token_delta(Q16Accumulator&, int frame, uint16_t token, int sign) const;
	void pair_delta(Q16Accumulator&, int frame, uint16_t a, uint16_t b, int sign) const;
	bool evaluate_reference(const Q16Accumulator&, unsigned side_to_move, Q16Result&, bool simd, std::string* error) const;
	bool evaluate_fast(const Q16Accumulator&, unsigned side_to_move, Q16Result* result, float* pre, std::string* error) const;

	// One dense layer's weights copied into 64-byte aligned memory in an order that makes the i32 partial sums
	// provably safe (see build_plan in q16.cpp).
	struct DensePlan {
		int rows = 0, cols = 0;
		AlignedBuffer<int16_t> packed;             // rows in plan order, groups of 4 rows contiguous
		struct Run { int chunk; int groups; };      // groups of 4 rows that use `chunk` madds per i32 accumulation
		std::vector<Run> runs;
		std::vector<uint16_t> row_of_slot;          // original row of each packed row
		int tail_rows = 0;                          // rows after the groups, computed one at a time
	};
	static bool build_plan(const int16_t* w, int rows, int cols, DensePlan& plan);

	void* mapping_ = nullptr;
	size_t bytes_ = 0;
	bool hidden2_ = false;
	bool checksum_verified_ = false;
	// Every table except pair is copied at load time into this 64-byte aligned block: SYMq16c's longer header puts
	// the arrays at odd file offsets, and int16/float arrays must not be read through misaligned pointers.
	AlignedBuffer<uint8_t> small_;
	// The 2,745,996 x 16 int16 pair rows, 32 bytes each, left in the mapping. They may start at any address (an odd
	// one for SYMq16c), so they are only read through unaligned loads / memcpy, never as an int16_t array.
	const uint8_t* pair_ = nullptr;
	const int16_t *embed_ = nullptr, *w1_ = nullptr, *w2_ = nullptr;
	const float *m_ = nullptr, *inv1_ = nullptr, *b1_ = nullptr, *a1_ = nullptr,
	            *inv2_ = nullptr, *b2_ = nullptr, *vw_ = nullptr, *vb_ = nullptr;
	DensePlan plan1_, plan2_;
};

// CRC-32C (Castagnoli): reflected polynomial 0x82F63B78, init and xorout ~0; "123456789" -> 0xE3069283.
// hardware selects the SSE4.2 path when this build has it (-mavx2 implies it); the result is identical either way.
uint32_t q16_crc32c(const void* data, size_t bytes, bool hardware = true);
bool q16_crc32c_has_hardware();

} } }
#endif
