#ifndef MANAKA_CORE_TOKENS_H_INCLUDED
#define MANAKA_CORE_TOKENS_H_INCLUDED

// BoardView から、2 つの枠(手番側と相手側ではなく、先手の枠と後手の枠)のトークン id の集合を作る。
//
// 正本は Rust 側の crates/manaka-core/src/token.rs(Features::Tokens 空間)。
//
//   盤上の駒    id = (kind * 2 + rel) * 81 + 枠での升
//   手駒        id = 2268 + rel * 38 + (kHandMax の先頭から kind の手前までの和) + コピー番号
//   rel         その駒の持ち主が枠の側でなければ 1、枠の側なら 0
//   枠での升    先手の枠(frame 0)は sq のまま、後手の枠(frame 1)は 80 - sq
//
// 後手の枠は盤を 180 度回し、持ち主を反転して読む。手駒は 1 枚ごとに 1 トークンで、歩が 3 枚ならコピー番号 0、1、2。
// 手番はトークンの集合に入らない(どちらの枠を先に W1 へ入れるかだけが手番を表す)。

#include <cstdint>

#include "board.h"

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

constexpr int kBoardTokens        = kNumKinds * 2 * kNumSquares; // 2268
constexpr int kHandTokensPerOwner = 18 + 4 + 4 + 4 + 4 + 2 + 2;  // 38
constexpr int kTokenSpace         = kBoardTokens + 2 * kHandTokensPerOwner; // 2344

// 1 枠が持ちうるトークンの最大数。81 マスがすべて埋まり、持ち駒が上限まで立った場合(合法局面は 40 枚まで)。
constexpr int kMaxTokensPerFrame = kNumSquares + 2 * kHandTokensPerOwner;

// 手駒の kind ごとの、持ち主の手駒ブロックの中での先頭。
constexpr int kHandBase[kNumHandKinds] = { 0, 18, 22, 26, 30, 34, 36 };

// 2 つの枠のトークン id を ids[0](先手の枠)と ids[1](後手の枠)に書き、個数を返す(両枠で同じ個数)。
// 並びは Rust の token::tokens と同じ: 盤の升番号の昇順、続いて先手の手駒(歩から飛、コピー番号の昇順)、後手の手駒。
// ids[f][i] は 2 つの枠で同じ駒を指す。BoardView が範囲外なら -1(ids の中身は不定)。
int collect_tokens(const BoardView& b, uint16_t ids[2][kMaxTokensPerFrame]);

// 組 {a, b}(a != b)の pair 表の行番号。三角形の表で、引数の順に依らない。
inline uint32_t pair_index(uint32_t a, uint32_t b) {
	const uint32_t low = a < b ? a : b;
	const uint32_t high = a < b ? b : a;
	return low * (2u * kTokenSpace - low - 1u) / 2u + (high - low - 1u);
}

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu

#endif // MANAKA_CORE_TOKENS_H_INCLUDED
