// core の単体試験。外部のファイルを要さず、合成した重みで動く(Rust の正解との突き合わせは eval_dump と compare_tsv.py)。
//
//   build/selftest
//
// 見るもの
//   - pair_index が三角形の表の全行を 1 度ずつ指す。トークン id の式が token.rs の文面どおりの値になる。
//   - YaneuraOu との対応表が、駒の名前から独立に導いた値と一致する。
//   - 先手後手を入れ替えた局面で、トークンの集合が一致する(T6)。
//   - SFEN の読み取りが正しく、壊れた SFEN を拒否する。
//   - MNW1 の読み込み: 切り詰め、末尾の余り、magic、版、未知の flags、対象外の形、NaN と Inf、形と大きさの食い違いを拒否する。
//     ヘッダーのでたらめな値でクラッシュしない。
//   - 合成した重みで、評価が倍精度の素朴な計算(初等対称式は DP で、Newton を使わない)と一致する。形は pair の有無、
//     h2 の有無、order 1 から 4、8 の倍数でない次元を含む。mmap とコピーで結果が同じ。
//   - 範囲外の BoardView は評価を拒否する。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <unistd.h>

#include "../eval_f32.h"
#include "sfen_parse.h"

using namespace YaneuraOu::Eval::ManakaCore;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond) \
	do { \
		++g_checks; \
		if (!(cond)) { \
			++g_failures; \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define CHECK_MSG(cond, msg) \
	do { \
		++g_checks; \
		if (!(cond)) { \
			++g_failures; \
			std::fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, std::string(msg).c_str()); \
		} \
	} while (0)

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

struct Rng {
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
	uint64_t next() {
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		return s;
	}
	// [-1, 1)
	float uniform() { return float(double(next() >> 11) / double(1ull << 53) * 2.0 - 1.0); }
	uint32_t below(uint32_t n) { return uint32_t(next() % n); }
};

// ---------------------------------------------------------------- pair_index

void test_pair_index() {
	uint32_t expect = 0;
	bool ok = true;
	for (uint32_t a = 0; a < kTokenSpace && ok; ++a) {
		for (uint32_t b = a + 1; b < kTokenSpace; ++b) {
			if (pair_index(a, b) != expect || pair_index(b, a) != expect) {
				ok = false;
				break;
			}
			++expect;
		}
	}
	CHECK(ok);
	CHECK(expect == kNumPairs);
	CHECK(kNumPairs == 2745996u);
	CHECK(kTokenSpace == 2344);
}

// ---------------------------------------------------------------- 対応表

void test_conversion_tables() {
	// YaneuraOu の PieceType の並びと Manaka の PIECE_KINDS の並びを、駒の名前で別々に書き、名前から対応を導く。
	static const char* const yane[16] = { "", "P", "L", "N", "S", "B", "R", "G", "K", "+P", "+L", "+N", "+S", "+B", "+R", "" };
	static const char* const manaka[kNumKinds] = { "P", "L", "N", "S", "G", "B", "R", "K", "+P", "+L", "+N", "+S", "+B", "+R" };
	for (int pt = 0; pt < 16; ++pt) {
		int want = -1;
		for (int k = 0; k < kNumKinds; ++k)
			if (yane[pt][0] != '\0' && std::strcmp(yane[pt], manaka[k]) == 0)
				want = k;
		CHECK_MSG(kKindFromYaneuraOuPieceType[pt] == want, "PieceType " + std::to_string(pt));
	}
	// 手駒: YaneuraOu は歩 1 から金 7。Manaka は歩 香 桂 銀 金 角 飛。
	static const char* const yane_hand[8] = { "", "P", "L", "N", "S", "B", "R", "G" };
	static const char* const manaka_hand[kNumHandKinds] = { "P", "L", "N", "S", "G", "B", "R" };
	for (int pt = 1; pt <= 7; ++pt) {
		int want = -1;
		for (int k = 0; k < kNumHandKinds; ++k)
			if (std::strcmp(yane_hand[pt], manaka_hand[k]) == 0)
				want = k;
		CHECK_MSG(hand_kind_from_yaneuraou_piece_type(pt) == want, "hand PieceType " + std::to_string(pt));
	}
	CHECK(hand_kind_from_yaneuraou_piece_type(0) == -1);
	CHECK(hand_kind_from_yaneuraou_piece_type(8) == -1);

	// Piece: 先手 = PieceType、後手 = PieceType + 16。
	CHECK(piece_code_from_yaneuraou_piece(0) == 0);
	CHECK(piece_code_from_yaneuraou_piece(1) == make_piece_code(0, kKindPawn));
	CHECK(piece_code_from_yaneuraou_piece(7) == make_piece_code(0, kKindGold));
	CHECK(piece_code_from_yaneuraou_piece(8) == make_piece_code(0, kKindKing));
	CHECK(piece_code_from_yaneuraou_piece(14) == make_piece_code(0, kKindProRook));
	CHECK(piece_code_from_yaneuraou_piece(16 + 5) == make_piece_code(1, kKindBishop));
	CHECK(piece_code_from_yaneuraou_piece(16 + 13) == make_piece_code(1, kKindProBishop));
	CHECK(piece_code_from_yaneuraou_piece(15) == -1);
	CHECK(piece_code_from_yaneuraou_piece(16) == -1);
	CHECK(piece_code_from_yaneuraou_piece(32) == -1);
	// 駒コードは 1 から 28 を過不足なく使う。
	bool used[kNumPieceCodes] = {};
	for (int pc = 0; pc < 32; ++pc) {
		const int code = piece_code_from_yaneuraou_piece(pc);
		if (code > 0) {
			CHECK(!used[code]);
			used[code] = true;
		}
	}
	for (int code = 1; code < kNumPieceCodes; ++code)
		CHECK(used[code]);

	// 升: SQ_11 = 0、sq = (筋 - 1) * 9 + (段 - 1) から (9 - 筋) * 9 + (段 - 1)。全単射。
	bool seen[kNumSquares] = {};
	for (int file = 1; file <= 9; ++file) {
		for (int rank = 1; rank <= 9; ++rank) {
			const int y = (file - 1) * 9 + (rank - 1);
			const int m = manaka_square_from_yaneuraou_square(y);
			CHECK(m == (9 - file) * 9 + (rank - 1));
			CHECK(!seen[m]);
			seen[m] = true;
		}
	}
	// 後手の枠で 80 - sq を取るのは、YaneuraOu の Inv(sq) = 80 - sq を先に取っても同じ。
	for (int y = 0; y < kNumSquares; ++y)
		CHECK(manaka_square_from_yaneuraou_square(80 - y) == 80 - manaka_square_from_yaneuraou_square(y));
}

// ---------------------------------------------------------------- トークン

BoardView must_parse(const char* sfen) {
	BoardView b;
	std::string err;
	const bool ok = parse_sfen(sfen, &b, &err);
	CHECK_MSG(ok, std::string(sfen) + ": " + err);
	if (!ok)
		b.clear();
	return b;
}

void test_sfen_and_tokens() {
	const BoardView init = must_parse("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1");
	// 先手の歩 1 七(筋 1、段 7): sq = (9 - 1) * 9 + 6 = 78。
	CHECK(init.cell[78] == make_piece_code(0, kKindPawn));
	// 後手の香 1 一: sq = 8 * 9 + 0 = 72。先手の香 9 九: sq = 0 * 9 + 8 = 8。
	CHECK(init.cell[72] == make_piece_code(1, kKindLance));
	CHECK(init.cell[8] == make_piece_code(0, kKindLance));
	// 先手の玉 5 九: sq = 4 * 9 + 8 = 44。
	CHECK(init.cell[44] == make_piece_code(0, kKindKing));
	CHECK(init.side_to_move == 0);

	uint16_t ids[2][kMaxTokensPerFrame];
	int n = collect_tokens(init, ids);
	CHECK(n == 40);
	// 先手の歩(筋 1 段 7、sq 78)。先手の枠: rel 0、升 78。後手の枠: rel 1、升 2。
	int found = 0;
	for (int i = 0; i < n; ++i) {
		if (ids[0][i] == (0 * 2 + 0) * 81 + 78) {
			CHECK(ids[1][i] == (0 * 2 + 1) * 81 + 2);
			++found;
		}
	}
	CHECK(found == 1);
	for (int f = 0; f < 2; ++f) {
		std::vector<int> v(ids[f], ids[f] + n);
		std::sort(v.begin(), v.end());
		CHECK(std::adjacent_find(v.begin(), v.end()) == v.end()); // 同じ id が 2 度出ない
		CHECK(v.back() < kTokenSpace);
	}

	// 持ち駒: 先手が歩 3 枚と飛 1 枚、後手が飛 2 枚と歩 1 枚。盤は玉だけ。
	const BoardView h = must_parse("4k4/9/9/9/9/9/9/9/4K4 w 3PR2rp 1");
	CHECK(h.side_to_move == 1);
	n = collect_tokens(h, ids);
	CHECK(n == 2 + 3 + 1 + 2 + 1);
	// 並び: 盤(升の昇順) 玉(後手 sq 36、先手 sq 44)、変換後は 先手の持ち駒(歩 0 1 2、飛 0)、変換後は 後手の持ち駒(歩 0、飛 0 1)。
	// 先手の枠: 持ち主が先手なら rel 0。先手の歩のコピー 0..2 は 2268 + 0 + 0 + c。先手の飛は 2268 + 0 + 36。後手の歩は 2268 + 38 + 0、後手の飛は 2268 + 38 + 36 + c。
	const uint16_t want0[] = {
		uint16_t((7 * 2 + 1) * 81 + 36), uint16_t((7 * 2 + 0) * 81 + 44), // 後手の玉 sq 36(rel 1)、先手の玉 sq 44(rel 0)
		2268 + 0, 2268 + 1, 2268 + 2, 2268 + 36,
		2268 + 38 + 0, 2268 + 38 + 36, 2268 + 38 + 37,
	};
	// 後手の枠: 盤を回し(80 - sq)、持ち主を反転。後手の玉 sq 36、変換後は 44 で rel 0、先手の玉 sq 44、変換後は 36 で rel 1。
	const uint16_t want1[] = {
		uint16_t((7 * 2 + 0) * 81 + 44), uint16_t((7 * 2 + 1) * 81 + 36),
		2268 + 38 + 0, 2268 + 38 + 1, 2268 + 38 + 2, 2268 + 38 + 36,
		2268 + 0, 2268 + 36, 2268 + 37,
	};
	CHECK(n == 9);
	for (int i = 0; i < 9; ++i) {
		CHECK_MSG(ids[0][i] == want0[i], "frame 0 token " + std::to_string(i) + ": " + std::to_string(ids[0][i]));
		CHECK_MSG(ids[1][i] == want1[i], "frame 1 token " + std::to_string(i) + ": " + std::to_string(ids[1][i]));
	}
	// 持ち駒の id の上限: 後手の飛 2 枚目が 2343。
	const BoardView full = must_parse("4k4/9/9/9/9/9/9/9/4K4 b 18P4L4N4S4G2B2R18p4l4n4s4g2b2r 1");
	n = collect_tokens(full, ids);
	CHECK(n == 2 + 76);
	int max_id = 0;
	for (int i = 0; i < n; ++i)
		max_id = std::max<int>(max_id, std::max(ids[0][i], ids[1][i]));
	CHECK(max_id == kTokenSpace - 1);

	// 範囲外の BoardView は -1。
	BoardView bad = init;
	bad.cell[0] = 29;
	CHECK(collect_tokens(bad, ids) == -1);
	bad = init;
	bad.hand[0][kKindPawn] = 19;
	CHECK(collect_tokens(bad, ids) == -1);
	std::string err;
	CHECK(!validate_board(bad, &err));
	bad = init;
	bad.side_to_move = 2;
	CHECK(!validate_board(bad, &err));
	CHECK(validate_board(init, &err));
}

void test_sfen_rejects() {
	BoardView b;
	std::string err;
	const char* const bad[] = {
		"",
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b",     // 欄が足りない
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1 b - 1",            // 段が足りない
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL/9 b - 1", // 段が多い
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSN b - 1",   // 筋が足りない
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNLL b - 1", // 筋が多い
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL x - 1",  // 手番
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b K 1",  // 玉は持ち駒にならない
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b 19P 1", // 上限
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b 3 1",  // 数の後ろに駒が無い
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - x",  // 手数
		"+gnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1", // 金は成れない
		"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNx b - 1",  // 未知の駒
	};
	for (const char* s : bad)
		CHECK_MSG(!parse_sfen(s, &b, &err), s);
	// 手数は省略してよく、成駒と手駒は読める。
	CHECK(parse_sfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b -", &b, &err));
	CHECK(parse_sfen("l6nl/5+P1gk/2np1S3/p1p4Pp/3P2Sp1/1PPb2P1P/P5GS1/R8/LN4bKL w GR5pnsg 1", &b, &err));
	CHECK(b.hand[0][kKindGold] == 1 && b.hand[0][kKindRook] == 1 && b.hand[1][kKindPawn] == 5);
	// 2 段目 9 筋から 4 つ目... "5+P1gk": 筋 9 から 5 つ空き、筋 4 に と金、筋 3 空き、筋 2 に後手の金、筋 1 に後手の玉。
	CHECK(b.cell[(9 - 4) * 9 + 1] == make_piece_code(0, kKindProPawn));
	CHECK(b.cell[(9 - 2) * 9 + 1] == make_piece_code(1, kKindGold));
	CHECK(b.cell[(9 - 1) * 9 + 1] == make_piece_code(1, kKindKing));
}

// 先手と後手を入れ替えた局面(盤を 180 度回し、持ち主を反転し、持ち駒を入れ替え、手番を反転)で、
// 先手の枠のトークン id の集合が、元の後手の枠と一致する(T6 の前提)。
BoardView flip_board(const BoardView& b) {
	BoardView r;
	r.clear();
	for (int sq = 0; sq < kNumSquares; ++sq) {
		const int code = b.cell[sq];
		if (code != 0)
			r.cell[80 - sq] = uint8_t(make_piece_code(1 - owner_of_piece_code(code), kind_of_piece_code(code)));
	}
	for (int k = 0; k < kNumHandKinds; ++k) {
		r.hand[0][k] = b.hand[1][k];
		r.hand[1][k] = b.hand[0][k];
	}
	r.side_to_move = uint8_t(1 - b.side_to_move);
	return r;
}

BoardView random_board(Rng& rng) {
	BoardView b;
	b.clear();
	// 玉 2 枚と、ランダムな駒 20 から 38 枚。重なれば空きを探す。
	auto place = [&](int code) {
		for (int tries = 0; tries < 200; ++tries) {
			const int sq = int(rng.below(kNumSquares));
			if (b.cell[sq] == 0) {
				b.cell[sq] = uint8_t(code);
				return;
			}
		}
	};
	place(make_piece_code(0, kKindKing));
	place(make_piece_code(1, kKindKing));
	const int pieces = 20 + int(rng.below(19));
	for (int i = 0; i < pieces; ++i)
		place(make_piece_code(int(rng.below(2)), int(rng.below(kNumKinds - 1) < kKindKing ? rng.below(kKindKing) : 8 + rng.below(6))));
	for (int o = 0; o < 2; ++o)
		for (int k = 0; k < kNumHandKinds; ++k)
			if (rng.below(3) == 0)
				b.hand[o][k] = uint8_t(rng.below(uint32_t(kHandMax[k]) + 1));
	b.side_to_move = uint8_t(rng.below(2));
	return b;
}

void test_flip_symmetry_of_tokens() {
	Rng rng(7);
	for (int iter = 0; iter < 2000; ++iter) {
		const BoardView a = random_board(rng);
		const BoardView f = flip_board(a);
		uint16_t ia[2][kMaxTokensPerFrame], iflip[2][kMaxTokensPerFrame];
		const int na = collect_tokens(a, ia), nf = collect_tokens(f, iflip);
		CHECK(na == nf && na >= 0);
		for (int frame = 0; frame < 2; ++frame) {
			std::vector<int> x(ia[frame], ia[frame] + na), y(iflip[1 - frame], iflip[1 - frame] + nf);
			std::sort(x.begin(), x.end());
			std::sort(y.begin(), y.end());
			CHECK(x == y);
		}
	}
}

// ---------------------------------------------------------------- MNW1 の合成

struct Shape {
	int dim, order, hidden, pair_dim, hidden2;
	int moment_dim = 0, field_dim = 0, pair_lse = 0;
	uint16_t flags = 0;
};

// Rust の params.rs の書式を、読み込みとは別の式で数える(独立した 2 つ目の導出)。
uint64_t expected_floats(const Shape& s, uint64_t tokens = kTokenSpace, uint64_t pairs = kNumPairs) {
	const uint64_t head = s.hidden2 > 0 ? s.hidden2 : s.hidden;
	const uint64_t groups = (s.flags & kFlagSplit) ? 2 : 1;
	const bool ks = (s.flags & kFlagKeepScale) != 0;
	const uint64_t moment_len = uint64_t(s.moment_dim) * (s.moment_dim + 1) / 2;
	const uint64_t scale_len = ks ? groups * s.order + (s.pair_dim - s.pair_lse > 0) + (s.moment_dim > 0) + (s.field_dim > 0) : 0;
	const uint64_t block = groups * s.order * s.dim + s.pair_dim + moment_len + 81ull * s.field_dim + scale_len;
	uint64_t n = 0;
	n += tokens * s.dim;                      // embed
	n += uint64_t(s.order) * s.dim;           // gamma
	n += pairs * s.pair_dim;                  // pair
	n += s.pair_dim;                          // pair_gamma
	n += scale_len;                           // scale_gamma
	n += uint64_t(s.hidden) * 2 * block;      // w1
	n += s.hidden;                            // b1
	n += uint64_t(s.hidden2) * s.hidden;      // w2
	n += s.hidden2;                           // b2
	n += head + 1;                            // value_w, value_b
	n += 2187 * head + 2187;                  // policy, policy_b
	n += uint64_t(s.dim) * head + s.dim;      // policy_from, policy_from_gamma
	n += tokens * s.moment_dim + moment_len;  // moment_embed, moment_gamma
	n += tokens * 81ull * s.field_dim + s.field_dim; // field, field_gamma
	n += 27ull * s.field_dim;                 // policy_field
	return n;
}

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

std::vector<uint8_t> make_header(const Shape& s, uint16_t version = 5, uint32_t magic = kMnwMagic, uint32_t tokens = kTokenSpace, uint32_t pairs = kNumPairs) {
	std::vector<uint8_t> h;
	put32(h, magic);
	put16(h, version);
	put16(h, uint16_t(s.dim));
	put16(h, uint16_t(s.order));
	put16(h, uint16_t(s.hidden));
	put16(h, uint16_t(s.pair_dim));
	put16(h, uint16_t(s.hidden2));
	put16(h, uint16_t(s.moment_dim));
	put16(h, uint16_t(s.field_dim));
	put16(h, uint16_t(s.pair_lse));
	put16(h, s.flags);
	put32(h, tokens);
	put32(h, 2187);
	put32(h, pairs);
	return h;
}

// 合成した重み。値の範囲は、実際の重みに近い程度(embed は ±0.3、W1 は ±0.1、gamma は 1 の近く)。
struct Synth {
	std::vector<float> data; // 全配列(ヘッダー以外)を並べたもの
};

Synth make_weights(const Shape& s, uint64_t seed) {
	Synth w;
	Rng rng(seed);
	const uint64_t total = expected_floats(s);
	w.data.resize(total);
	// 配列の並びと値の範囲。
	const uint64_t head = s.hidden2 > 0 ? s.hidden2 : s.hidden;
	struct Part { uint64_t count; float amp; float bias; };
	const Part parts[] = {
		{ uint64_t(kTokenSpace) * s.dim, 0.3f, 0.0f },       // embed
		{ uint64_t(s.order) * s.dim, 0.2f, 1.0f },           // gamma
		{ uint64_t(kNumPairs) * s.pair_dim, 0.05f, 0.0f },   // pair
		{ uint64_t(s.pair_dim), 0.2f, 1.0f },                // pair_gamma
		{ uint64_t(s.hidden) * 2 * (uint64_t(s.order) * s.dim + s.pair_dim), 0.1f, 0.0f }, // w1
		{ uint64_t(s.hidden), 0.1f, 0.0f },                  // b1
		{ uint64_t(s.hidden2) * s.hidden, 0.1f, 0.0f },      // w2
		{ uint64_t(s.hidden2), 0.1f, 0.0f },                 // b2
		{ head, 0.2f, 0.0f },                                // value_w
		{ 1, 0.1f, 0.0f },                                   // value_b
	};
	uint64_t at = 0;
	for (const Part& p : parts) {
		for (uint64_t i = 0; i < p.count; ++i)
			w.data[at + i] = p.bias + p.amp * rng.uniform();
		at += p.count;
	}
	for (; at < total; ++at) // policy 系は評価に使わない
		w.data[at] = 0.5f * rng.uniform();
	return w;
}

std::string tmp_path(const char* tag) {
	const char* dir = std::getenv("TMPDIR");
	std::string p = std::string(dir ? dir : "/tmp") + "/manaka_core_selftest_" + std::to_string(::getpid()) + "_" + tag + ".mnw";
	return p;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& header, const std::vector<float>& data, size_t cut_from_end = 0, size_t extra = 0) {
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	std::vector<uint8_t> all(header);
	const uint8_t* d = reinterpret_cast<const uint8_t*>(data.data());
	all.insert(all.end(), d, d + data.size() * 4);
	all.resize(all.size() - cut_from_end);
	for (size_t i = 0; i < extra; ++i)
		all.push_back(0);
	const bool ok = std::fwrite(all.data(), 1, all.size(), f) == all.size();
	std::fclose(f);
	return ok;
}

// ---------------------------------------------------------------- 倍精度の素朴な評価

struct Naive {
	double pre = 0.0;
	std::vector<double> features;
};

Naive naive_evaluate(const Shape& s, const std::vector<float>& d, const BoardView& b) {
	const uint64_t embed_at = 0;
	const uint64_t gamma_at = embed_at + uint64_t(kTokenSpace) * s.dim;
	const uint64_t pair_at = gamma_at + uint64_t(s.order) * s.dim;
	const uint64_t pair_gamma_at = pair_at + uint64_t(kNumPairs) * s.pair_dim;
	const uint64_t block = uint64_t(s.order) * s.dim + s.pair_dim;
	const uint64_t w1_at = pair_gamma_at + s.pair_dim;
	const uint64_t b1_at = w1_at + uint64_t(s.hidden) * 2 * block;
	const uint64_t w2_at = b1_at + s.hidden;
	const uint64_t b2_at = w2_at + uint64_t(s.hidden2) * s.hidden;
	const uint64_t head = s.hidden2 > 0 ? s.hidden2 : s.hidden;
	const uint64_t vw_at = b2_at + s.hidden2;
	const uint64_t vb_at = vw_at + head;

	uint16_t ids[2][kMaxTokensPerFrame];
	const int n = collect_tokens(b, ids);
	Naive out;
	std::vector<double> feat;
	for (int half = 0; half < 2; ++half) {
		const int frame = half == 0 ? b.side_to_move : 1 - b.side_to_move;
		// 初等対称式を DP で: e[k] += e[k-1] * v。Newton の恒等式は使わない。
		for (int k = 1; k <= s.order; ++k) {
			std::vector<double> e(size_t(s.order) + 1);
			std::vector<double> ek(s.dim);
			for (int c = 0; c < s.dim; ++c) {
				std::fill(e.begin(), e.end(), 0.0);
				e[0] = 1.0;
				for (int t = 0; t < n; ++t) {
					const double v = d[embed_at + uint64_t(ids[frame][t]) * s.dim + c];
					for (int j = s.order; j >= 1; --j)
						e[j] += e[j - 1] * v;
				}
				ek[c] = e[k];
			}
			double ms = 0.0;
			for (double v : ek)
				ms += v * v;
			ms /= s.dim;
			const double scale = 1.0 / std::sqrt(ms + 1e-6);
			for (int c = 0; c < s.dim; ++c)
				feat.push_back(ek[c] * scale * d[gamma_at + uint64_t(k - 1) * s.dim + c]);
		}
		if (s.pair_dim > 0) {
			std::vector<double> q(s.pair_dim, 0.0);
			for (int i = 0; i < n; ++i)
				for (int j = i + 1; j < n; ++j)
					for (int c = 0; c < s.pair_dim; ++c)
						q[c] += d[pair_at + uint64_t(pair_index(ids[frame][i], ids[frame][j])) * s.pair_dim + c];
			double ms = 0.0;
			for (double v : q)
				ms += v * v;
			ms /= s.pair_dim;
			const double scale = 1.0 / std::sqrt(ms + 1e-6);
			for (int c = 0; c < s.pair_dim; ++c)
				feat.push_back(q[c] * scale * d[pair_gamma_at + c]);
		}
	}
	out.features = feat;
	std::vector<double> h(s.hidden);
	for (int r = 0; r < s.hidden; ++r) {
		double acc = d[b1_at + r];
		for (size_t i = 0; i < feat.size(); ++i)
			acc += double(d[w1_at + uint64_t(r) * feat.size() + i]) * feat[i];
		h[r] = acc > 0 ? acc : 0;
	}
	if (s.hidden2 > 0) {
		std::vector<double> h2(s.hidden2);
		for (int r = 0; r < s.hidden2; ++r) {
			double acc = d[b2_at + r];
			for (int i = 0; i < s.hidden; ++i)
				acc += double(d[w2_at + uint64_t(r) * s.hidden + i]) * h[i];
			h2[r] = acc > 0 ? acc : 0;
		}
		h = h2;
	}
	double pre = d[vb_at];
	for (uint64_t i = 0; i < head; ++i)
		pre += double(d[vw_at + i]) * h[i];
	out.pre = pre;
	return out;
}

// ---------------------------------------------------------------- 試験の本体

void test_header_only() {
	MnwHeader h;
	std::string err;
	const Shape base = { 128, 3, 512, 16, 0 };
	const uint64_t bytes = 36 + 4 * expected_floats(base);
	std::vector<uint8_t> hdr = make_header(base);
	CHECK(parse_mnw_header(hdr.data(), bytes, &h, &err));
	CHECK(h.feature_len == 800);
	CHECK(h.head_width == 512);
	CHECK(mnw_shape_name(h) == "token_pair16_d128_o3_h512");
	CHECK(mnw_unsupported_reason(h) == nullptr);
	// MNW1 の base 形はヘッダと全 f32 配列で 183,338,388 バイト。
	CHECK_MSG(bytes == 183338388ull, std::to_string(bytes));

	const Shape h2 = { 128, 3, 512, 16, 256 };
	hdr = make_header(h2);
	CHECK(parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(h2), &h, &err));
	CHECK(mnw_shape_name(h) == "token_pair16_d128_o3_h512_h2256");
	CHECK(h.head_width == 256);

	// 1 バイトでも違えば拒否。大きさを確保の前に見ている。
	CHECK(!parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(h2) - 1, &h, &err) && contains(err, "短い"));
	CHECK(!parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(h2) + 1, &h, &err) && contains(err, "余り"));
	CHECK(!parse_mnw_header(hdr.data(), 35, &h, &err) && contains(err, "ヘッダー"));
	CHECK(!parse_mnw_header(hdr.data(), 0, &h, &err));

	// magic、版
	hdr = make_header(base, 5, 0x12345678);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "MNW1 ではない"));
	for (uint16_t v : { uint16_t(0), uint16_t(1), uint16_t(4), uint16_t(6), uint16_t(65535) }) {
		hdr = make_header(base, v);
		CHECK_MSG(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "版"), std::to_string(v));
	}

	// 未知の flags
	Shape s = base;
	s.flags = 1u << 5;
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(s), &h, &err) && contains(err, "flags"));
	s.flags = uint16_t(1u << 15);
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(s), &h, &err) && contains(err, "flags"));
	s.flags = 3u << 1; // 特徴空間 3
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), 0xFFFFFFFFFFull, &h, &err));

	// 形の範囲
	s = base;
	s.order = 0;
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "order"));
	s.order = 5;
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "order"));
	s = base;
	s.dim = 0;
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "次元"));
	s = base;
	s.hidden = 0;
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "次元"));
	s = base;
	s.pair_lse = 17;
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "pair_lse"));
	hdr = make_header(base, 5, kMnwMagic, kTokenSpace + 1);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "tokens"));
	hdr = make_header(base, 5, kMnwMagic, kTokenSpace, kNumPairs - 1);
	CHECK(!parse_mnw_header(hdr.data(), bytes, &h, &err) && contains(err, "pair_rows"));

	// 対象外の形は、ヘッダーとしては正しく(大きさも合う)、理由つきで対象外になる。
	struct Unsupported { Shape s; const char* why; };
	Shape lse = base;
	lse.pair_lse = 4;
	Shape ks = base;
	ks.flags = kFlagKeepScale;
	Shape mom = base;
	mom.moment_dim = 4;
	Shape fld = base;
	fld.field_dim = 2;
	Shape spl = base;
	spl.flags = kFlagSplit;
	Shape nn = base;
	nn.flags = kFlagNnue;
	const Unsupported cases[] = { { lse, "lse" }, { ks, "ks" }, { mom, "m(" }, { fld, "f(" }, { spl, "split" }, { nn, "nnue" } };
	for (const Unsupported& c : cases) {
		hdr = make_header(c.s);
		const bool ok = parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(c.s), &h, &err);
		CHECK_MSG(ok, c.why + std::string(": ") + err);
		if (ok) {
			const char* why = mnw_unsupported_reason(h);
			CHECK_MSG(why != nullptr && contains(why, c.why), c.why);
		}
	}
	// 形が大きい場合の、大きさの導出が独立に数えた値と一致する(ks と m と f を含む)。
	Shape big = { 64, 4, 128, 8, 32 };
	big.moment_dim = 3;
	big.field_dim = 2;
	big.pair_lse = 2;
	big.flags = kFlagKeepScale;
	hdr = make_header(big);
	CHECK(parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(big), &h, &err));
	CHECK(h.scale_len == 4 + 1 + 1 + 1);

	// KP と HalfKP: pair_rows は 0、tokens は 1710 と 125389。
	Shape kp = { 16, 2, 32, 0, 0 };
	kp.flags = uint16_t(1u << 1);
	hdr = make_header(kp, 5, kMnwMagic, 1710, 0);
	CHECK(parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(kp, 1710, 0), &h, &err));
	CHECK(mnw_unsupported_reason(h) != nullptr);
	kp.flags = uint16_t(2u << 1);
	hdr = make_header(kp, 5, kMnwMagic, 125389, 0);
	CHECK(parse_mnw_header(hdr.data(), 36 + 4 * expected_floats(kp, 125389, 0), &h, &err));
	CHECK(mnw_unsupported_reason(h) != nullptr);

	// 巨大な次元の主張は、ファイルが小さければ確保の前に拒否される(桁あふれもしない)。
	s = { 65535, 4, 65535, 65535, 65535 };
	s.moment_dim = 65535;
	s.field_dim = 65535;
	hdr = make_header(s);
	CHECK(!parse_mnw_header(hdr.data(), 1000, &h, &err));
	CHECK(!parse_mnw_header(hdr.data(), 0xFFFFFFFFFFFFFFFFull, &h, &err));

	// でたらめなヘッダーでクラッシュしない。受理されたものは、全配列がファイルの中に収まり隙間なく並ぶ。
	Rng rng(99);
	int accepted = 0;
	for (int iter = 0; iter < 200000; ++iter) {
		uint8_t raw[36];
		for (uint8_t& x : raw)
			x = uint8_t(rng.next());
		// 先頭を MNW1 と版 5 にして、中身の検査に届かせる。値は小さめに寄せる。
		raw[0] = 'M'; raw[1] = 'N'; raw[2] = 'W'; raw[3] = '1'; raw[4] = 5; raw[5] = 0;
		for (int o = 6; o < 24; o += 2)
			if (rng.below(4) != 0)
				raw[o + 1] = 0;
		if (rng.below(2)) {
			raw[24] = uint8_t(kTokenSpace); raw[25] = uint8_t(kTokenSpace >> 8); raw[26] = 0; raw[27] = 0;
			raw[28] = uint8_t(2187); raw[29] = uint8_t(2187 >> 8); raw[30] = 0; raw[31] = 0;
			raw[32] = uint8_t(kNumPairs); raw[33] = uint8_t(kNumPairs >> 8); raw[34] = uint8_t(kNumPairs >> 16); raw[35] = 0;
		}
		MnwHeader hh;
		const uint64_t fb = (rng.below(3) == 0) ? rng.next() : (36 + rng.below(1u << 20) * 4);
		if (parse_mnw_header(raw, fb, &hh, &err)) {
			++accepted;
			uint64_t cursor = 36;
			bool tight = true;
			for (int i = 0; i < kNumMnwSections; ++i) {
				tight = tight && hh.section[i].offset == cursor;
				cursor += hh.section[i].count * 4;
			}
			CHECK(tight && cursor == fb);
		}
	}
	std::printf("  random garbage headers: %d accepted of 200000\n", accepted);

	// 妥当な範囲のでたらめな形(lse、ks、m、f、split、nnue、KP、HalfKP を含む)で、読み込みの大きさの導出が、この試験の別の導出と
	// 一致する。受理されるべき形はちょうどの大きさで受理され、1 バイトずれれば(増減とも)拒否される。
	int agree = 0, rejected_shapes = 0;
	for (int iter = 0; iter < 20000; ++iter) {
		Shape r = { 1 + int(rng.below(300)), 1 + int(rng.below(4)), 1 + int(rng.below(600)), int(rng.below(41)), int(rng.below(3) ? 0 : 1 + rng.below(300)) };
		r.moment_dim = rng.below(4) ? 0 : int(1 + rng.below(6));
		r.field_dim = rng.below(4) ? 0 : int(1 + rng.below(4));
		r.pair_lse = rng.below(3) ? 0 : int(rng.below(uint32_t(r.pair_dim) + 3));
		const int features = rng.below(3) ? 0 : int(1 + rng.below(2));
		r.flags = uint16_t((rng.below(3) == 0 ? kFlagKeepScale : 0) | (features << 1) | (rng.below(5) == 0 ? kFlagNnue : 0) | (rng.below(5) == 0 ? kFlagSplit : 0));
		const uint32_t tokens = features == 0 ? kTokenSpace : (features == 1 ? 1710u : 125389u);
		const uint32_t pairs = features == 0 ? kNumPairs : 0;
		const bool valid = r.pair_lse <= r.pair_dim && !((r.flags & kFlagSplit) && features != 0) && !((r.flags & kFlagSplit) && (r.flags & kFlagNnue));
		const std::vector<uint8_t> hh = make_header(r, 5, kMnwMagic, tokens, pairs);
		const uint64_t want = 36 + 4 * expected_floats(r, tokens, pairs);
		MnwHeader parsed;
		const bool ok = parse_mnw_header(hh.data(), want, &parsed, &err);
		CHECK_MSG(ok == valid, err);
		if (valid) {
			CHECK(!parse_mnw_header(hh.data(), want + 4, &parsed, &err));
			CHECK(!parse_mnw_header(hh.data(), want - 4, &parsed, &err));
			CHECK(!parse_mnw_header(hh.data(), want + 1, &parsed, &err));
			CHECK(!parse_mnw_header(hh.data(), want - 1, &parsed, &err));
			++agree;
		} else {
			++rejected_shapes;
		}
	}
	std::printf("  random shapes: %d agree with the independent size count, %d invalid shapes rejected\n", agree, rejected_shapes);
}

void test_load_failures() {
	const Shape s = { 8, 3, 16, 2, 0 };
	const Synth w = make_weights(s, 5);
	const std::vector<uint8_t> hdr = make_header(s);
	const std::string path = tmp_path("fail");
	MnwWeights out;
	std::string err;
	MnwLoadOptions opt;

	CHECK(write_file(path, hdr, w.data));
	CHECK_MSG(MnwWeights::load(path, opt, &out, &err), err);
	CHECK(out.pair_is_mapped());
	MnwWeights moved(std::move(out));
	CHECK(moved.pair_is_mapped() && moved.pair() != nullptr);

	// 切り詰め、末尾の余り
	CHECK(write_file(path, hdr, w.data, 1));
	CHECK(!MnwWeights::load(path, opt, &out, &err) && contains(err, "短い"));
	CHECK(write_file(path, hdr, w.data, 0, 1));
	CHECK(!MnwWeights::load(path, opt, &out, &err) && contains(err, "余り"));
	CHECK(write_file(path, hdr, w.data, 4 * 2187 * 8)); // policy を丸ごと落とした
	CHECK(!MnwWeights::load(path, opt, &out, &err));

	// 存在しない
	CHECK(!MnwWeights::load(path + ".nonexistent", opt, &out, &err));

	// 先頭 1 バイトの改変
	std::vector<uint8_t> bad = hdr;
	bad[0] ^= 1;
	CHECK(write_file(path, bad, w.data));
	CHECK(!MnwWeights::load(path, opt, &out, &err) && contains(err, "MNW1 ではない"));

	// 対象外の形
	{
		Shape lse = s;
		lse.pair_lse = 1;
		const Synth wl = make_weights(lse, 6);
		CHECK(write_file(path, make_header(lse), wl.data));
		CHECK(!MnwWeights::load(path, opt, &out, &err) && contains(err, "lse"));
		Shape ks = s;
		ks.flags = kFlagKeepScale;
		const Synth wk = make_weights(ks, 6);
		CHECK(write_file(path, make_header(ks), wk.data));
		CHECK(!MnwWeights::load(path, opt, &out, &err) && contains(err, "ks"));
	}

	// NaN と Inf: policy を含む全配列で拒否する。
	const uint64_t embed_at = 0;
	const uint64_t gamma_at = embed_at + uint64_t(kTokenSpace) * s.dim;
	const uint64_t pair_at = gamma_at + uint64_t(s.order) * s.dim;
	const uint64_t pair_gamma_at = pair_at + uint64_t(kNumPairs) * s.pair_dim;
	const uint64_t w1_at = pair_gamma_at + s.pair_dim;
	const uint64_t block = uint64_t(s.order) * s.dim + s.pair_dim;
	const uint64_t b1_at = w1_at + uint64_t(s.hidden) * 2 * block;
	const uint64_t value_w_at = b1_at + s.hidden;
	const uint64_t value_b_at = value_w_at + s.hidden;
	const uint64_t policy_at = value_b_at + 1;
	const float nan = std::nanf("");
	const float inf = INFINITY;
	struct Poison { const char* name; uint64_t at; float v; bool rejected; };
	const Poison poisons[] = {
		{ "embed NaN", embed_at + 5, nan, true },
		{ "gamma Inf", gamma_at + 1, inf, true },
		{ "pair NaN", pair_at + 1000, nan, true },
		{ "pair -Inf", pair_at + uint64_t(kNumPairs) * s.pair_dim - 1, -inf, true },
		{ "pair_gamma NaN", pair_gamma_at, nan, true },
		{ "w1 NaN", w1_at + 3, nan, true },
		{ "b1 Inf", b1_at, inf, true },
		{ "value_w NaN", value_w_at + 2, nan, true },
		{ "value_b NaN", value_b_at, nan, true },
		{ "policy NaN", policy_at + 10, nan, true },
	};
	for (const Poison& p : poisons) {
		Synth bad_w = w;
		bad_w.data[p.at] = p.v;
		CHECK(write_file(path, hdr, bad_w.data));
		MnwLoadOptions o = opt;
		const bool loaded = MnwWeights::load(path, o, &out, &err);
		CHECK_MSG(loaded == !p.rejected, p.name);
		if (p.rejected)
			CHECK_MSG(contains(err, "NaN"), p.name);
		o.mmap_pair = false;
		CHECK_MSG(MnwWeights::load(path, o, &out, &err) == !p.rejected, p.name);
	}
	std::remove(path.c_str());
}

bool same_bits(float a, float b) {
	uint32_t x, y;
	std::memcpy(&x, &a, 4);
	std::memcpy(&y, &b, 4);
	return x == y;
}

void test_evaluation(const Shape& s, const char* name, uint64_t seed, double tolerance) {
	const Synth w = make_weights(s, seed);
	const std::string path = tmp_path(name);
	CHECK(write_file(path, make_header(s), w.data));

	MnwWeights mapped, copied;
	std::string err;
	MnwLoadOptions opt;
	CHECK_MSG(MnwWeights::load(path, opt, &mapped, &err), err);
	opt.mmap_pair = false;
	CHECK_MSG(MnwWeights::load(path, opt, &copied, &err), err);
	CHECK(mapped.pair_is_mapped() == (s.pair_dim > 0));
	CHECK(!copied.pair_is_mapped());
	auto ev_m = EvaluatorF32::create(&mapped, &err);
	auto ev_c = EvaluatorF32::create(&copied, &err);
	CHECK(ev_m && ev_c);
	if (!ev_m || !ev_c) {
		std::remove(path.c_str());
		return;
	}

	Rng rng(seed * 31 + 1);
	double worst_pre = 0.0, worst_feat = 0.0;
	const int positions = 150;
	for (int iter = 0; iter < positions; ++iter) {
		const BoardView b = random_board(rng);
		EvaluatorF32::Result rm, rc;
		CHECK(ev_m->evaluate(b, &rm));
		CHECK(ev_c->evaluate(b, &rc));
		CHECK(same_bits(rm.pre, rc.pre) && same_bits(rm.value, rc.value)); // mmap とコピーで同じ
		CHECK(same_bits(rm.value, ::tanhf(rm.pre)));

		const Naive nv = naive_evaluate(s, w.data, b);
		worst_pre = std::max(worst_pre, std::fabs(nv.pre - double(rm.pre)));

		std::vector<float> feat(ev_m->header().feature_len);
		CHECK(ev_m->build_features(b, feat.data()));
		CHECK(feat.size() == nv.features.size());
		for (size_t i = 0; i < feat.size(); ++i)
			worst_feat = std::max(worst_feat, std::fabs(nv.features[i] - double(feat[i])));
		// forward(features) が evaluate の pre と一致する。
		CHECK(same_bits(ev_m->forward(feat.data()), rm.pre));
	}
	std::printf("  %-26s worst |pre - naive| %.3e, worst feature %.3e\n", name, worst_pre, worst_feat);
	CHECK_MSG(worst_pre < tolerance, std::string(name) + " pre " + std::to_string(worst_pre));
	CHECK_MSG(worst_feat < tolerance, std::string(name) + " feature " + std::to_string(worst_feat));

	// 範囲外の BoardView は評価を拒否する。
	BoardView bad = random_board(rng);
	bad.cell[3] = 29;
	EvaluatorF32::Result r;
	r.pre = 123.0f;
	CHECK(!ev_m->evaluate(bad, &r));
	CHECK(r.pre == 123.0f);
	bad = random_board(rng);
	bad.hand[1][kKindBishop] = 3;
	CHECK(!ev_m->evaluate(bad, &r));
	bad = random_board(rng);
	bad.side_to_move = 7;
	CHECK(!ev_m->evaluate(bad, &r));

	std::remove(path.c_str());
}

// 先手後手を入れ替えた局面は、同じ特徴ベクトルと同じ評価になる(T6)。f32 の足し算の順序が変わるので、ビット一致ではなく近さを見る。
void test_flip_symmetry_of_evaluation() {
	const Shape s = { 16, 3, 32, 4, 8 };
	const Synth w = make_weights(s, 77);
	const std::string path = tmp_path("flip");
	CHECK(write_file(path, make_header(s), w.data));
	MnwWeights weights;
	std::string err;
	CHECK_MSG(MnwWeights::load(path, MnwLoadOptions(), &weights, &err), err);
	auto ev = EvaluatorF32::create(&weights, &err);
	CHECK(ev != nullptr);
	if (ev) {
		Rng rng(5);
		double worst = 0.0;
		for (int iter = 0; iter < 300; ++iter) {
			const BoardView a = random_board(rng);
			const BoardView f = flip_board(a);
			EvaluatorF32::Result ra, rf;
			CHECK(ev->evaluate(a, &ra) && ev->evaluate(f, &rf));
			worst = std::max(worst, std::fabs(double(ra.pre) - double(rf.pre)));
		}
		std::printf("  flip symmetry: worst |pre(P) - pre(flip P)| %.3e\n", worst);
		CHECK(worst < 1e-4);
	}
	std::remove(path.c_str());
}

// 有限の値だけで埋まったモデルでも、f32 の中間値があふれる可能性があればロードを拒否する。
void test_arithmetic_bounds() {
	const Shape s = { 8, 3, 16, 2, 8 };
	const Synth w = make_weights(s, 97);
	const std::string path = tmp_path("overflow");
	const auto hdr = make_header(s);
	MnwHeader h;
	std::string err;
	CHECK(parse_mnw_header(hdr.data(), 36 + 4 * w.data.size(), &h, &err));
	const int stages[] = { kSecEmbed, kSecGamma, kSecPair, kSecPairGamma, kSecW1, kSecB1, kSecW2, kSecB2, kSecValueW, kSecValueB };
	for (int section : stages) {
		Synth poisoned = w;
		const size_t at = size_t((h.section[section].offset - 36) / 4);
		for (uint64_t i = 0; i < h.section[section].count; ++i)
			poisoned.data[at + i] = std::numeric_limits<float>::max();
		CHECK(write_file(path, hdr, poisoned.data));
		MnwWeights out;
		CHECK_MSG(!MnwWeights::load(path, MnwLoadOptions(), &out, &err), "section " + std::to_string(section));
		CHECK_MSG(contains(err, "安全上界"), err);
	}
	// P_j 自体は有限でも RMS の平方和があふれる場合も拒否する。
	Synth large = w;
	for (size_t i = 0; i < uint64_t(kTokenSpace) * s.dim; ++i) large.data[i] = 1e10f;
	CHECK(write_file(path, hdr, large.data));
	MnwWeights out;
	CHECK(!MnwWeights::load(path, MnwLoadOptions(), &out, &err));
	CHECK_MSG(contains(err, "安全上界"), err);
	std::remove(path.c_str());
}

void verify_stale_evaluator(const std::string& base, const std::string& h2256) {
	MnwWeights weights;
	std::string err;
	CHECK_MSG(MnwWeights::load(base, MnwLoadOptions(), &weights, &err), err);
	auto ev = EvaluatorF32::create(&weights, &err);
	CHECK(ev != nullptr);
	if (!ev) return;
	const uint64_t revision = weights.revision();
	const BoardView board = must_parse("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1");
	EvaluatorF32::Result r;
	CHECK(ev->evaluate(board, &r));
	std::vector<float> features(weights.header().feature_len, 777.0f);
	CHECK_MSG(MnwWeights::load(h2256, MnwLoadOptions(), &weights, &err), err);
	CHECK(weights.revision() != revision);
	r.pre = 123.0f;
	CHECK(!ev->evaluate(board, &r));
	CHECK(r.pre == 123.0f);
	CHECK(!ev->build_features(board, features.data()));
	CHECK(features[0] == 777.0f);
	CHECK(std::isnan(ev->forward(features.data())));
	auto fresh = EvaluatorF32::create(&weights, &err);
	CHECK(fresh && fresh->evaluate(board, &r));
	// move した元と、move 代入で置換された先も、古い評価器を拒否する。
	auto source_ev = EvaluatorF32::create(&weights, &err);
	MnwWeights moved(std::move(weights));
	CHECK(!source_ev->evaluate(board, &r));
	CHECK(EvaluatorF32::create(&weights, &err) == nullptr);
	CHECK(weights.header().version == 0);
	auto moved_ev = EvaluatorF32::create(&moved, &err);
	CHECK(moved_ev && moved_ev->evaluate(board, &r));
	CHECK_MSG(MnwWeights::load(base, MnwLoadOptions(), &weights, &err), err);
	auto target_ev = EvaluatorF32::create(&weights, &err);
	weights = std::move(moved);
	CHECK(!target_ev->evaluate(board, &r));
	CHECK(!moved_ev->evaluate(board, &r));
	CHECK(EvaluatorF32::create(&moved, &err) == nullptr);
	CHECK(moved.header().version == 0);
}

void test_weight_replacement() {
	const Shape base = { 8, 3, 512, 0, 0 };
	const Shape h2 = { 8, 3, 512, 0, 256 };
	const std::string p1 = tmp_path("reload_base"), p2 = tmp_path("reload_h2256");
	CHECK(write_file(p1, make_header(base), make_weights(base, 121).data));
	CHECK(write_file(p2, make_header(h2), make_weights(h2, 122).data));
	verify_stale_evaluator(p1, p2);
	std::remove(p1.c_str());
	std::remove(p2.c_str());
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 1 && argc != 3) {
		std::fprintf(stderr, "usage: selftest [BASE.mnw H2256.mnw]\n");
		return 2;
	}
	std::printf("pair_index\n");
	test_pair_index();
	std::printf("conversion tables\n");
	test_conversion_tables();
	std::printf("sfen and tokens\n");
	test_sfen_and_tokens();
	test_sfen_rejects();
	test_flip_symmetry_of_tokens();
	std::printf("mnw header\n");
	test_header_only();
	std::printf("mnw load failures\n");
	test_load_failures();
	test_arithmetic_bounds();
	test_weight_replacement();
	if (argc == 3) verify_stale_evaluator(argv[1], argv[2]);
	std::printf("evaluation against a naive double computation\n");
	test_evaluation({ 8, 3, 16, 2, 0 }, "d8 o3 h16 pair2", 11, 1e-3);
	test_evaluation({ 16, 3, 24, 8, 12 }, "d16 o3 h24 pair8 h2 12", 12, 1e-3);
	test_evaluation({ 5, 2, 13, 3, 0 }, "d5 o2 h13 pair3 (odd)", 13, 1e-3);
	test_evaluation({ 7, 4, 9, 16, 5 }, "d7 o4 h9 pair16 h2 5", 14, 1e-3);
	test_evaluation({ 8, 1, 8, 0, 0 }, "d8 o1 h8 pair0", 15, 1e-3);
	test_evaluation({ 24, 3, 32, 64, 0 }, "d24 o3 h32 pair64", 16, 1e-3);
	test_evaluation({ 12, 2, 10, 20, 7 }, "d12 o2 h10 pair20 h2 7", 17, 1e-3);
	test_flip_symmetry_of_evaluation();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
