#include "hce_speed.h"

#if defined(USE_HCE_EXPLAIN)

#include <deque>
#include <vector>

#include "../../movegen.h"
#include "../../position.h"

namespace YaneuraOu {
namespace Eval {
namespace Hce {

const char* const kSpeedNames[NUM_SPEED] = {
	"mover.checks",  "mover.safe_checks",  "mover.mate1",  "mover.mate3",
	"waiter.checks", "waiter.safe_checks", "waiter.mate1", "waiter.mate3",
};

namespace {

// 合法手を全部。不成も含める (explain の指し手の受け取りと同じ)。
std::vector<Move> legal_moves(const Position& pos) {
	std::vector<Move> out;
	for (const Move m : MoveList<LEGAL_ALL>(pos))
		out.push_back(m);
	return out;
}

// 手番側に、王手をかけて相手の合法手を無くす手があるか。
bool mates_in_one(Position& pos) {
	StateInfo st;
	for (const Move m : legal_moves(pos)) {
		if (!pos.gives_check(m))
			continue;
		pos.do_move(m, st);
		const bool mated = MoveList<LEGAL_ALL>(pos).size() == 0;
		pos.undo_move(m);
		if (mated)
			return true;
	}
	return false;
}

// 手番側が攻める側として見た 4 つの数。
void attack(Position& pos, int* out) {
	std::vector<Move> checks;
	for (const Move m : legal_moves(pos))
		if (pos.gives_check(m))
			checks.push_back(m);

	int  safe  = 0;
	bool mate1 = false;
	bool mate3 = false;
	StateInfo st1, st2;
	for (const Move m : checks) {
		pos.do_move(m, st1);
		const std::vector<Move> replies = legal_moves(pos);
		const Square to = m.to_sq();
		bool recapture = false;
		for (const Move e : replies)
			if (e.to_sq() == to) { recapture = true; break; }
		if (!recapture)
			++safe;
		if (replies.empty())
			mate1 = true;
		// どの応手に対しても 1 手詰めが残る王手は 3 手詰めの一手目。
		// 応手が無ければ 1 手詰めで、all_of が真になるが、mate1 が先に立つので同じ結果になる。
		if (!mate3) {
			bool forced = true;
			for (const Move e : replies) {
				pos.do_move(e, st2);
				const bool mates = mates_in_one(pos);
				pos.undo_move(e);
				if (!mates) { forced = false; break; }
			}
			if (forced)
				mate3 = true;
		}
		pos.undo_move(m);
	}
	out[0] = int(checks.size());
	out[1] = safe;
	out[2] = mate1 ? 1 : 0;
	out[3] = (mate1 || mate3) ? 1 : 0;
}

} // namespace

void speed_features(Position& pos, int* out) {
	for (int i = 0; i < NUM_SPEED; ++i)
		out[i] = 0;

	const Color mover = pos.side_to_move();
	const int   sign_mover = mover == BLACK ? 1 : -1;

	int a[4];
	attack(pos, a);
	for (int i = 0; i < 4; ++i)
		out[i] = sign_mover * a[i];

	// 王手をかけられているときは手番を渡せない。
	if (!pos.in_check()) {
		StateInfo st;
		pos.do_null_move(st);
		attack(pos, a);
		pos.undo_null_move();
		for (int i = 0; i < 4; ++i)
			out[4 + i] = -sign_mover * a[i];
	}
}

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
