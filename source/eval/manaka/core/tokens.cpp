#include "tokens.h"

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

int collect_tokens(const BoardView& b, uint16_t ids[2][kMaxTokensPerFrame]) {
	int n = 0;

	for (int sq = 0; sq < kNumSquares; ++sq) {
		const int code = b.cell[sq];
		if (code == 0)
			continue;
		if (code >= kNumPieceCodes)
			return -1;
		const int owner = owner_of_piece_code(code);
		const int kind = kind_of_piece_code(code);
		// 先手の枠では持ち主が後手なら相手、後手の枠では持ち主が先手なら相手。
		ids[0][n] = uint16_t((kind * 2 + owner) * kNumSquares + sq);
		ids[1][n] = uint16_t((kind * 2 + (1 - owner)) * kNumSquares + (kNumSquares - 1 - sq));
		++n;
	}

	for (int owner = 0; owner < 2; ++owner) {
		for (int kind = 0; kind < kNumHandKinds; ++kind) {
			const int count = b.hand[owner][kind];
			if (count > kHandMax[kind])
				return -1;
			const int base0 = kBoardTokens + owner * kHandTokensPerOwner + kHandBase[kind];
			const int base1 = kBoardTokens + (1 - owner) * kHandTokensPerOwner + kHandBase[kind];
			for (int copy = 0; copy < count; ++copy) {
				ids[0][n] = uint16_t(base0 + copy);
				ids[1][n] = uint16_t(base1 + copy);
				++n;
			}
		}
	}
	return n;
}

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu
