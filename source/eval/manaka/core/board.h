#ifndef MANAKA_CORE_BOARD_H_INCLUDED
#define MANAKA_CORE_BOARD_H_INCLUDED

#include <cstdint>
#include <cstring>
#include <string>

#include "common.h"

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

// --- 駒の種類(kind)
//
// Manaka の token.rs の PIECE_KINDS の順。成駒は別の kind として持つ(YaneuraOu の BonaPiece のように金へ畳まない)。
// 手駒になれる種類(HAND_KINDS)は歩から飛までで、その番号は kind の番号と同じ。
constexpr int kKindPawn      = 0;
constexpr int kKindLance     = 1;
constexpr int kKindKnight    = 2;
constexpr int kKindSilver    = 3;
constexpr int kKindGold      = 4;
constexpr int kKindBishop    = 5;
constexpr int kKindRook      = 6;
constexpr int kKindKing      = 7;
constexpr int kKindProPawn   = 8;
constexpr int kKindProLance  = 9;
constexpr int kKindProKnight = 10;
constexpr int kKindProSilver = 11;
constexpr int kKindProBishop = 12; // 馬
constexpr int kKindProRook   = 13; // 龍
constexpr int kNumKinds      = 14;
constexpr int kNumHandKinds  = 7;

constexpr int kNumSquares = 81;

// 持ち駒の種類ごとの上限(歩 香 桂 銀 金 角 飛)。token.rs の HAND_MAX。
constexpr int kHandMax[kNumHandKinds] = { 18, 4, 4, 4, 4, 2, 2 };

// --- 盤面の入力
//
// 評価に要る情報だけを持つ。YaneuraOu の Position を知らない。
//
//   cell[sq]  : Manaka の升番号 sq = (9 - 筋) * 9 + (段 - 1) の順に 81 マス。
//               0 は空き。それ以外は 駒コード = 1 + owner * 14 + kind。
//               owner は 0 が先手、1 が後手。kind は上の kKind*。
//   hand      : hand[owner][手駒の種類]。種類の順は 歩 香 桂 銀 金 角 飛。
//   side_to_move : 0 が先手番、1 が後手番。
//
// 先手の駒コードは 1 から 14、後手は 15 から 28。
struct BoardView {
	uint8_t cell[kNumSquares];
	uint8_t hand[2][kNumHandKinds];
	uint8_t side_to_move;

	void clear() { std::memset(this, 0, sizeof(*this)); }
};

constexpr int kNumPieceCodes = 1 + 2 * kNumKinds; // 空き + 先手 14 + 後手 14

constexpr int make_piece_code(int owner, int kind) { return 1 + owner * kNumKinds + kind; }
constexpr int owner_of_piece_code(int code) { return (code - 1) / kNumKinds; }
constexpr int kind_of_piece_code(int code) { return (code - 1) % kNumKinds; }

// BoardView の値が範囲内かどうか。範囲外ならトークンの添字が壊れるので、評価の前に検査する。
// 駒の数が合法局面(40 枚)でなくても、玉が無くても受け付ける(Rust の manaka-core も検査しない)。
inline bool validate_board(const BoardView& b, std::string* err) {
	for (int sq = 0; sq < kNumSquares; ++sq) {
		if (b.cell[sq] >= kNumPieceCodes) {
			if (err) *err = "cell[" + std::to_string(sq) + "] = " + std::to_string(b.cell[sq]) + " は駒コードの範囲外";
			return false;
		}
	}
	for (int o = 0; o < 2; ++o) {
		for (int k = 0; k < kNumHandKinds; ++k) {
			if (b.hand[o][k] > kHandMax[k]) {
				if (err) *err = "hand[" + std::to_string(o) + "][" + std::to_string(k) + "] = " + std::to_string(b.hand[o][k]) + " は上限 " + std::to_string(kHandMax[k]) + " を超える";
				return false;
			}
		}
	}
	if (b.side_to_move > 1) {
		if (err) *err = "side_to_move は 0 か 1";
		return false;
	}
	return true;
}

// --- YaneuraOu との対応表
//
// YaneuraOu の型には依存せず、整数の値だけで対応を書く。値は source/types.h の
//   PieceType : 歩 1 香 2 桂 3 銀 4 角 5 飛 6 金 7 玉 8 と 9 成香 10 成桂 11 成銀 12 馬 13 龍 14
//   Piece     : 先手の駒 = PieceType、後手の駒 = PieceType + 16(PIECE_WHITE)、NO_PIECE = 0
//   Square    : SQ_11 = 0、sq = (筋 - 1) * 9 + (段 - 1)
// による。手駒は YaneuraOu では歩 1 から金 7 の PieceType で数える(PIECE_HAND_ZERO = PAWN)。
// 金が飛の後ろに来るので、角と飛と金の番号が Manaka と違う。

// PieceType から kind。-1 は対応が無い(NO_PIECE_TYPE と QUEEN と範囲外)。
constexpr int8_t kKindFromYaneuraOuPieceType[16] = { -1, 0, 1, 2, 3, 5, 6, 4, 7, 8, 9, 10, 11, 12, 13, -1 };

// YaneuraOu の Piece の値から駒コード。NO_PIECE は 0、対応が無い値は -1。
constexpr int piece_code_from_yaneuraou_piece(int pc) {
	if (pc == 0)
		return 0;
	if (pc < 0 || pc >= 32)
		return -1;
	const int kind = kKindFromYaneuraOuPieceType[pc & 15];
	return kind < 0 ? -1 : make_piece_code((pc >> 4) & 1, kind);
}

// YaneuraOu の手駒の PieceType(歩 1 から金 7)から手駒の種類の番号(Manaka の歩 香 桂 銀 金 角 飛 の順)。範囲外は -1。
constexpr int hand_kind_from_yaneuraou_piece_type(int pt) {
	return (pt >= 1 && pt <= 7) ? kKindFromYaneuraOuPieceType[pt] : -1;
}

// YaneuraOu の Square の値から Manaka の升番号。後手の枠は Manaka 側で 80 - sq を取るので、ここでは考えない。
constexpr int manaka_square_from_yaneuraou_square(int sq) { return (8 - sq / 9) * 9 + sq % 9; }

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu

#endif // MANAKA_CORE_BOARD_H_INCLUDED
