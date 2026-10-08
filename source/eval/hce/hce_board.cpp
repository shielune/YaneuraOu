#include "hce_board.h"

#if defined(USE_HCE_EXPLAIN)

#include <cstring>

#include "../../position.h"

namespace YaneuraOu {
namespace Eval {
namespace Hce {

namespace {

// 学習側の駒の種類 K R B G S N L P +R +B +S +N +L +P の番号。
constexpr int kPackedKind[PIECE_TYPE_NB] = {
	-1,  // NO_PIECE_TYPE
	 7,  // PAWN
	 6,  // LANCE
	 5,  // KNIGHT
	 4,  // SILVER
	 2,  // BISHOP
	 1,  // ROOK
	 3,  // GOLD
	 0,  // KING
	13,  // PRO_PAWN
	12,  // PRO_LANCE
	11,  // PRO_KNIGHT
	10,  // PRO_SILVER
	 9,  // HORSE
	 8,  // DRAGON
	-1,
};

// 利きの種類の集合に使う番号 (歩 香 桂 銀 角 飛 金 玉 馬 龍。成香などは金に入れる)。
constexpr int kAttackKind[PIECE_TYPE_NB] = {
	-1, 0, 1, 2, 3, 4, 5, 6, 7, 6, 6, 6, 6, 8, 9, -1,
};

// 持ち駒の並び 歩 香 桂 銀 金 角 飛。
constexpr PieceType kHandOrder[7] = { PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK };

// エンジンの升から学習側の升へ。筋が逆向き。
inline int learn_square(Square sq) {
	const int engine = int(sq);
	return (8 - engine / 9) * 9 + engine % 9;
}

} // namespace

void board_features(const Position& pos, uint8_t* out) {
	std::memset(out, 0, NUM_BOARD);

	uint8_t  count[2][81] = {};
	uint16_t mask[2][81]  = {};
	Bitboard attacks[SQ_NB];

	Bitboard all = pos.pieces();
	while (all) {
		const Square sq = all.pop();
		const Piece  pc = pos.piece_on(sq);
		const Bitboard atk = effects_from(pc, sq, pos.pieces());
		attacks[int(sq)] = atk;
		const int side = color_of(pc) == BLACK ? 0 : 1;
		const int kind = kAttackKind[type_of(pc)];
		Bitboard t = atk;
		while (t) {
			const int target = learn_square(t.pop());
			++count[side][target];
			mask[side][target] |= uint16_t(1) << kind;
		}
	}

	for (int e = 0; e < int(SQ_NB); ++e) {
		const Square sq = Square(e);
		const int    at = learn_square(sq) * PER_SQUARE;
		const int    ls = learn_square(sq);
		out[at + 1] = count[0][ls];
		out[at + 2] = count[1][ls];
		out[at + 3] = uint8_t(mask[0][ls] & 0xFF);
		out[at + 4] = uint8_t(mask[0][ls] >> 8);
		out[at + 5] = uint8_t(mask[1][ls] & 0xFF);
		out[at + 6] = uint8_t(mask[1][ls] >> 8);

		const Piece pc = pos.piece_on(sq);
		if (pc == NO_PIECE)
			continue;
		const int colour = color_of(pc) == BLACK ? 0 : 1;
		out[at] = uint8_t(1 + kPackedKind[type_of(pc)] + 14 * colour);

		int empty = 0, own = 0, enemy = 0;
		uint16_t kinds = 0;
		Bitboard t = attacks[e];
		while (t) {
			const Square target = t.pop();
			const Piece  tp = pos.piece_on(target);
			if (tp == NO_PIECE) {
				++empty;
			} else if (color_of(tp) == color_of(pc)) {
				++own;
			} else {
				++enemy;
				kinds |= uint16_t(1) << kAttackKind[type_of(tp)];
			}
		}
		out[at + 7]  = uint8_t(empty);
		out[at + 8]  = uint8_t(own);
		out[at + 9]  = uint8_t(enemy);
		out[at + 10] = uint8_t(kinds & 0xFF);
		out[at + 11] = uint8_t(kinds >> 8);
	}

	const int hands = 81 * PER_SQUARE;
	for (int side = 0; side < 2; ++side)
		for (int k = 0; k < 7; ++k)
			out[hands + side * 7 + k] = uint8_t(hand_count(pos.hand_of(side == 0 ? BLACK : WHITE), kHandOrder[k]));
	out[hands + 14] = pos.side_to_move() == BLACK ? 0 : 1;
}

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
