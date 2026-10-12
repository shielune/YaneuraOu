#include "hce_progress.h"

#if defined(USE_HCE_EXPLAIN)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include "../../position.h"

namespace YaneuraOu {
namespace Eval {
namespace Hce {

namespace {

constexpr char     kMagic[8]    = { 'H', 'C', 'E', 'P', 'R', 'G', '0', '1' };
constexpr int      kClasses     = 28;            // 手番側の駒 14 種 + 相手の駒 14 種
constexpr int      kPlain       = kClasses * 81; // 升と駒の入力数
constexpr int      kHandInputs  = 14;
constexpr uint32_t kHeaderBytes = 32;
// 持ち駒は 歩 香 桂 銀 金 角 飛 の個数を、持てる最大数 18 4 4 4 4 2 2 で割る。
constexpr float kHandMax[7] = { 18.0f, 4.0f, 4.0f, 4.0f, 4.0f, 2.0f, 2.0f };

// 学習側の駒の番号 (下位 4 ビット) K R B G S N L P +R +B +S +N +L +P から、
// 駒の種類への対応。
constexpr PieceType kKindOfCode[14] = {
	KING, ROOK, BISHOP, GOLD, SILVER, KNIGHT, LANCE, PAWN,
	DRAGON, HORSE, PRO_SILVER, PRO_KNIGHT, PRO_LANCE, PRO_PAWN,
};

// エンジンの駒の種類から学習側の番号 (下位 4 ビット) を引く表。
struct CodeTable {
	int code[PIECE_TYPE_NB];
	constexpr CodeTable() : code() {
		for (int i = 0; i < PIECE_TYPE_NB; ++i)
			code[i] = -1;
		for (int c = 0; c < 14; ++c)
			code[kKindOfCode[c]] = c;
	}
};
constexpr CodeTable kCode;

// 持ち駒の並び 歩 香 桂 銀 金 角 飛。
constexpr PieceType kHandOrder[7] = { PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK };

// 学習側の升 (ファイル 9 から 1、段 1 から 9) とエンジンの升 (ファイル 1 から 9) の変換。
// 学習側は (9 - file) * 9 + (rank - 1)、エンジンは (file - 1) * 9 + (rank - 1)。
inline int engine_square_of(int learn) { return (8 - learn / 9) * 9 + learn % 9; }
inline int learn_square_of(int engine) { return (8 - engine / 9) * 9 + engine % 9; }

bool read_u32(std::istream& in, uint32_t& v) {
	unsigned char b[4];
	if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
	v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
	return true;
}

} // namespace

bool Progress::load(const std::string& path, std::string& error) {
	auto bad = [&](const std::string& why) { error = why; return false; };
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) return bad("failed to open " + path);
	const std::streamoff length = file.tellg();
	if (length < std::streamoff(kHeaderBytes)) return bad("file is shorter than its header");
	const size_t size = size_t(length);
	std::vector<unsigned char> bytes(size);
	file.seekg(0);
	if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
		return bad("read failed");
	if (std::memcmp(bytes.data(), kMagic, 8)) return bad("wrong magic");

	auto word = [&](size_t at) {
		const unsigned char* b = bytes.data() + at;
		return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
	};
	const uint32_t version = word(8), plain = word(12), hidden = word(16), second = word(20);
	const uint32_t scale_bits = word(24), reserved = word(28);
	float scale;
	std::memcpy(&scale, &scale_bits, 4);
	if (version != 1 || plain != uint32_t(kPlain) || reserved != 0)
		return bad("unsupported version or input width");
	if (hidden == 0 || hidden > 4096 || second == 0 || second > 4096)
		return bad("hidden width out of range");
	if (!(scale > 0.0f) || scale > 1e6f) return bad("invalid output scale");

	const size_t count = size_t(kPlain) * hidden + size_t(kHandInputs) * hidden + hidden
	                   + size_t(second) * hidden + second + second + 1;
	if (bytes.size() != kHeaderBytes + 4 * count) return bad("unexpected payload length");
	static_assert(sizeof(float) == 4, "float32 required");
	std::vector<float> v(count);
	for (size_t i = 0; i < count; ++i) {
		const uint32_t bits = word(kHeaderBytes + 4 * i);
		// -ffast-math の下でも非有限値を弾けるよう、ビットで見る。
		if ((bits & 0x7f800000u) == 0x7f800000u) return bad("nonfinite weight");
		std::memcpy(&v[i], &bits, 4);
	}

	size_t at = 0;
	auto take = [&](std::vector<float>& out, size_t n) {
		out.assign(v.begin() + at, v.begin() + at + n);
		at += n;
	};
	take(ps_, size_t(kPlain) * hidden);
	take(hand_w_, size_t(kHandInputs) * hidden);
	take(hand_b_, hidden);
	take(l1_w_, size_t(second) * hidden);
	take(l1_b_, second);
	take(out_w_, second);
	out_b_ = v[at];

	hidden_ = int(hidden);
	second_ = int(second);
	scale_ = scale;
	// ファイル全体の FNV-1a。
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
	fingerprint_ = h;
	return true;
}

float Progress::moves_left(const Position& pos) const {
	if (!loaded()) return 0.0f;

	const Color  us   = pos.side_to_move();
	const bool   flip = us == WHITE;
	std::vector<float> acc(hand_b_);   // 持ち駒の層の偏りから始める

	// 升と駒: 手番側から見て、自駒は 0..13、敵駒は 14..27。後手番のときは盤を回す。
	Bitboard all = pos.pieces();
	while (all) {
		const Square sq = all.pop();
		const Piece  pc = pos.piece_on(sq);
		const int    code = kCode.code[type_of(pc)];
		const int    cls  = (color_of(pc) == us ? 0 : 14) + code;
		int          learn = learn_square_of(int(sq));
		if (flip) learn = 80 - learn;
		const float* row = &ps_[size_t(cls * 81 + learn) * hidden_];
		for (int h = 0; h < hidden_; ++h) acc[h] += row[h];
	}

	// 持ち駒: 手番側 7 個、相手側 7 個の順で、持てる最大数で割った値。
	float hands[kHandInputs];
	for (int i = 0; i < 7; ++i) {
		hands[i]     = float(hand_count(pos.hand_of(us),  kHandOrder[i])) / kHandMax[i];
		hands[7 + i] = float(hand_count(pos.hand_of(~us), kHandOrder[i])) / kHandMax[i];
	}
	for (int i = 0; i < kHandInputs; ++i) {
		if (hands[i] == 0.0f) continue;
		const float* row = &hand_w_[size_t(i) * hidden_];
		for (int h = 0; h < hidden_; ++h) acc[h] += hands[i] * row[h];
	}
	for (int h = 0; h < hidden_; ++h) acc[h] = std::max(acc[h], 0.0f);

	float out = out_b_;
	for (int s = 0; s < second_; ++s) {
		const float* row = &l1_w_[size_t(s) * hidden_];
		float sum = l1_b_[s];
		for (int h = 0; h < hidden_; ++h) sum += row[h] * acc[h];
		out += out_w_[s] * std::max(sum, 0.0f);
	}
	return std::max(out * scale_, 1.0f);
}

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
