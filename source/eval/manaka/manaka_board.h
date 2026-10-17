#ifndef EVAL_MANAKA_MANAKA_BOARD_H_INCLUDED
#define EVAL_MANAKA_MANAKA_BOARD_H_INCLUDED

// YaneuraOu の Position から core の BoardView を作る。
//
// 変換の表 (駒種、升、手駒の並び) は core/board.h にある。ここは Position の読み出しだけを持つ。
// core は YaneuraOu の型を知らないので、Piece、Square、PieceType は整数にして渡す。

#include "../../config.h"

#if defined(EVAL_MANAKA)

#include "../../position.h"
#include "../../types.h"
#include "core/board.h"

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

// pos の盤と持ち駒と手番を board に写す。
// 盤の駒が Manaka の kind に対応しない値 (QUEEN など) だったら false を返す。合法な Position では起きない。
// 王手中でも、玉が盤上に無くても、駒の数が 40 でなくても、そのまま写す (評価の側は検査しない)。
inline bool make_board_view(const Position& pos, ManakaCore::BoardView* board) {
	using namespace ManakaCore;

	board->clear();

	for (int sq = 0; sq < SQ_NB; ++sq) {
		const Piece pc = pos.piece_on(static_cast<Square>(sq));
		if (pc == NO_PIECE)
			continue;
		const int code = piece_code_from_yaneuraou_piece(static_cast<int>(pc));
		if (code < 0)
			return false;
		board->cell[manaka_square_from_yaneuraou_square(sq)] = static_cast<uint8_t>(code);
	}

	for (int c = 0; c < COLOR_NB; ++c) {
		const Hand hand = pos.hand_of(static_cast<Color>(c));
		for (int pt = PAWN; pt <= GOLD; ++pt) {
			const int kind = hand_kind_from_yaneuraou_piece_type(pt);
			board->hand[c][kind] = static_cast<uint8_t>(hand_count(hand, static_cast<PieceType>(pt)));
		}
	}

	board->side_to_move = static_cast<uint8_t>(pos.side_to_move());
	return true;
}

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)

#endif // EVAL_MANAKA_MANAKA_BOARD_H_INCLUDED
