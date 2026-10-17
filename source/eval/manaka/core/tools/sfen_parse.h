#ifndef MANAKA_CORE_TOOLS_SFEN_PARSE_H_INCLUDED
#define MANAKA_CORE_TOOLS_SFEN_PARSE_H_INCLUDED

// テスト用ドライバのための小さな SFEN の読み取り。core 本体には含めない(エンジンは自前の Position を持つ)。
//
// 通常の SFEN: "<盤> <手番> <持ち駒> [手数]"。盤は段 1 から 9、各段は筋 9 から 1。
// 手数は読み飛ばす。Rust の dump_eval と違い、1 段が 9 筋ちょうどでなければ拒否する。

#include <cstdint>
#include <string>
#include <vector>

#include "../board.h"

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

namespace sfen_detail {

// SFEN の基本の駒の文字から kind(成りなし)。対応が無ければ -1。玉と金は成れない。
inline int kind_from_char(char upper) {
	switch (upper) {
	case 'P': return kKindPawn;
	case 'L': return kKindLance;
	case 'N': return kKindKnight;
	case 'S': return kKindSilver;
	case 'G': return kKindGold;
	case 'B': return kKindBishop;
	case 'R': return kKindRook;
	case 'K': return kKindKing;
	default: return -1;
	}
}

inline int promoted_kind(int kind) {
	switch (kind) {
	case kKindPawn: return kKindProPawn;
	case kKindLance: return kKindProLance;
	case kKindKnight: return kKindProKnight;
	case kKindSilver: return kKindProSilver;
	case kKindBishop: return kKindProBishop;
	case kKindRook: return kKindProRook;
	default: return -1;
	}
}

} // namespace sfen_detail

// 成功すれば true。失敗したら理由を *err に書く。
inline bool parse_sfen(const std::string& text, BoardView* out, std::string* err) {
	using namespace sfen_detail;
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};

	std::vector<std::string> parts;
	{
		size_t i = 0;
		while (i < text.size()) {
			while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
				++i;
			size_t j = i;
			while (j < text.size() && text[j] != ' ' && text[j] != '\t')
				++j;
			if (j > i)
				parts.push_back(text.substr(i, j - i));
			i = j;
		}
	}
	if (parts.size() < 3 || parts.size() > 4)
		return fail("SFEN は 3 か 4 つの欄(盤 手番 持ち駒 [手数])");

	BoardView b;
	b.clear();

	// 盤
	{
		const std::string& s = parts[0];
		int rank = 1, file = 9;
		for (size_t i = 0; i < s.size(); ++i) {
			const char c = s[i];
			if (c == '/') {
				if (file != 0)
					return fail("段 " + std::to_string(rank) + " が 9 筋に足りない");
				if (++rank > 9)
					return fail("段が 9 を超える");
				file = 9;
			} else if (c >= '1' && c <= '9') {
				file -= c - '0';
				if (file < 0)
					return fail("段 " + std::to_string(rank) + " が 9 筋を超える");
			} else {
				bool promote = false;
				char ch = c;
				if (ch == '+') {
					promote = true;
					if (++i >= s.size())
						return fail("'+' の後ろに駒が無い");
					ch = s[i];
				}
				const bool black = (ch >= 'A' && ch <= 'Z');
				const char upper = black ? ch : char(ch - 'a' + 'A');
				int kind = kind_from_char(upper);
				if (kind < 0)
					return fail(std::string("盤に未知の駒 '") + ch + "'");
				if (promote) {
					kind = promoted_kind(kind);
					if (kind < 0)
						return fail(std::string("成れない駒 '+") + ch + "'");
				}
				if (file <= 0)
					return fail("段 " + std::to_string(rank) + " が 9 筋を超える");
				const int sq = (9 - file) * 9 + (rank - 1);
				b.cell[sq] = uint8_t(make_piece_code(black ? 0 : 1, kind));
				--file;
			}
		}
		if (rank != 9 || file != 0)
			return fail("盤が 9 段 x 9 筋でない");
	}

	// 手番
	if (parts[1] == "b")
		b.side_to_move = 0;
	else if (parts[1] == "w")
		b.side_to_move = 1;
	else
		return fail("手番は b か w: " + parts[1]);

	// 持ち駒
	if (parts[2] != "-") {
		const std::string& s = parts[2];
		size_t i = 0;
		while (i < s.size()) {
			int count = 1;
			if (s[i] >= '0' && s[i] <= '9') {
				count = 0;
				while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
					count = count * 10 + (s[i] - '0');
					if (count > 99)
						return fail("持ち駒の数が大きすぎる");
					++i;
				}
				if (count == 0)
					return fail("持ち駒の数が 0");
			}
			if (i >= s.size())
				return fail("持ち駒の数の後ろに駒が無い");
			const char ch = s[i++];
			const bool black = (ch >= 'A' && ch <= 'Z');
			const char upper = black ? ch : char(ch - 'a' + 'A');
			const int kind = kind_from_char(upper);
			if (kind < 0 || kind > kKindRook)
				return fail(std::string("持ち駒に置けない駒 '") + ch + "'");
			const int total = b.hand[black ? 0 : 1][kind] + count;
			if (total > kHandMax[kind])
				return fail(std::string("持ち駒 '") + ch + "' が上限 " + std::to_string(kHandMax[kind]) + " を超える");
			b.hand[black ? 0 : 1][kind] = uint8_t(total);
		}
	}

	// 手数
	if (parts.size() == 4) {
		for (char c : parts[3])
			if (c < '0' || c > '9')
				return fail("手数が数でない: " + parts[3]);
	}

	*out = b;
	return true;
}

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu

#endif // MANAKA_CORE_TOOLS_SFEN_PARSE_H_INCLUDED
