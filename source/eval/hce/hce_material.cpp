#include "hce_material.h"

#if defined(USE_HCE_EXPLAIN)

#include "../../position.h"

namespace YaneuraOu {
namespace Eval {
namespace Hce {

namespace {

// 駒の種類から列への対応。-1 は数えない (玉)。
// 成香・成桂・成銀・と金は動きが金と同じなので 7 番に入れる。
constexpr int kBoardColumn[PIECE_TYPE_NB] = {
	-1, 0, 1, 2, 3, 5, 6, 4, -1, 7, 7, 7, 7, 8, 9, -1,
};
// 持ち駒は 歩 香 桂 銀 金 角 飛 の順で 10 列目から。
constexpr PieceType kHandOrder[7] = { PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK };

} // namespace

void material_counts(const Position& pos, int* count) {
	for (int i = 0; i < NUM_MATERIAL_COLUMNS; ++i)
		count[i] = 0;

	Bitboard all = pos.pieces();
	while (all) {
		const Square sq = all.pop();
		const Piece  pc = pos.piece_on(sq);
		const int col   = kBoardColumn[type_of(pc)];
		if (col >= 0)
			count[col] += color_of(pc) == BLACK ? 1 : -1;
	}
	for (int i = 0; i < 7; ++i)
		count[10 + i] = hand_count(pos.hand_of(BLACK), kHandOrder[i])
		              - hand_count(pos.hand_of(WHITE), kHandOrder[i]);
}

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
