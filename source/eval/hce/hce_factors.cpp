#include "hce_factors.h"

#if defined(USE_HCE_EXPLAIN)

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace YaneuraOu {
namespace Eval {
namespace Hce {

namespace {

// 学習側の升は (9 - 筋) x 9 + (段 - 1)。
inline int file_of_learn(int s) { return 9 - s / 9; }
inline int rank_of_learn(int s) { return s % 9 + 1; }

inline int find_king(const uint8_t* board, int colour) {
	const uint8_t code = uint8_t(1 + 14 * colour);
	for (int s = 0; s < 81; ++s)
		if (board[s * PER_SQUARE] == code)
			return s;
	return 0;
}

} // namespace

void king_window(const uint8_t* board, int colour, int radius, float* out) {
	const int width = window_width(radius);
	std::fill(out, out + width, 0.0f);

	const int ksq = find_king(board, colour);
	const int kf = file_of_learn(ksq), kr = rank_of_learn(ksq);
	const int sign = colour == 0 ? 1 : -1;

	int cell = 0;
	for (int df = -radius; df <= radius; ++df) {
		for (int dr = -radius; dr <= radius; ++dr, ++cell) {
			float* c = out + cell * WINDOW_CELL;
			const int f = kf - sign * df;
			const int r = kr - sign * dr;
			const bool on = f >= 1 && f <= 9 && r >= 1 && r <= 9;
			if (!on) {
				c[31] = 1.0f;
				continue;
			}
			const int idx = (9 - f) * 9 + (r - 1);
			const uint8_t* sq = board + idx * PER_SQUARE;
			const int o = sq[0];
			int rel = 0;
			if (o != 0) {
				const int owner = (o - 1) / 14, kind = (o - 1) % 14;
				rel = owner == colour ? kind + 1 : kind + 15;
			}
			c[rel] = 1.0f;
			c[29] = float(sq[1 + colour]) / 4.0f;
			c[30] = float(sq[2 - colour]) / 4.0f;
		}
	}
	// 相手の持ち駒。
	const uint8_t* hands = board + 81 * PER_SQUARE + (1 - colour) * 7;
	for (int k = 0; k < 7; ++k)
		out[cell * WINDOW_CELL + k] = float(hands[k]) / 4.0f;
}

int piece_facts(const uint8_t* board, std::vector<float>& facts, std::vector<float>& sign) {
	facts.clear();
	sign.clear();
	const int king[2] = { find_king(board, 0), find_king(board, 1) };
	int count = 0;
	for (int s = 0; s < 81; ++s) {
		const uint8_t* sq = board + s * PER_SQUARE;
		const int o = sq[0];
		if (o == 0)
			continue;
		const int kind = (o - 1) % 14, colour = (o - 1) / 14;
		if (kind == 0)
			continue;   // 玉は数えない
		const bool white = colour == 1;
		const int own_att = white ? sq[2] : sq[1];
		const int foe_att = white ? sq[1] : sq[2];
		const int rank = rank_of_learn(s), file = file_of_learn(s);
		const int own_rank = white ? 10 - rank : rank;
		auto dist = [&](int c) {
			return std::max(std::abs(file - file_of_learn(king[c])), std::abs(rank - rank_of_learn(king[c])));
		};
		const int own_king = white ? dist(1) : dist(0);
		const int foe_king = white ? dist(0) : dist(1);

		const size_t at = facts.size();
		facts.resize(at + PIECE_FACTS, 0.0f);
		float* f = &facts[at];
		int n = 0;
		f[n + kind] = 1.0f;                       n += 14;
		f[n++] = float(sq[7]) / 4.0f;             // 空きの升への利き
		f[n++] = float(sq[8]) / 4.0f;             // 自駒への利き
		f[n++] = float(sq[9]) / 4.0f;             // 敵駒への利き
		const int kinds = sq[10] + (sq[11] << 8);
		for (int b = 0; b < 10; ++b) f[n + b] = float((kinds >> b) & 1);
		n += 10;
		f[n++] = float(own_att) / 4.0f;
		f[n++] = float(foe_att) / 4.0f;
		f[n++] = foe_att > own_att ? 1.0f : 0.0f;
		f[n + std::min(own_king, 8)] = 1.0f;      n += 9;
		f[n + std::min(foe_king, 8)] = 1.0f;      n += 9;
		f[n + (own_rank - 1)] = 1.0f;             n += 9;
		sign.push_back(white ? -1.0f : 1.0f);
		++count;
	}
	return count;
}

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
