#include "../../config.h"

#if defined(EVAL_MANAKA)

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "core/q16.h"
#include "manaka_net.h"

// nn.bin (SYMq16c と旧 SYMq16) の読み込み口と、整数評価 (core/q16.cpp) への接続。
// 形式の検査は core/q16.cpp の読み込みが持つ。ここは薄い橋だけを持つ。
//
// Q16Network は読み込んだあとは変わらず const のメソッドだけで評価できるので、スレッド間で共有する。
// 作業用の領域は Q16Result (約 9 KB) で、Session が持つ。

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

namespace {

#ifndef MANAKA_RING_REMEMBER_UPDATES
#define MANAKA_RING_REMEMBER_UPDATES 1
#endif
class Q16Session final : public Session {
public:
	explicit Q16Session(const ManakaCore::Q16Network* net) : net_(net) {}

	bool incremental() const override { return true; }

	bool rebuild(const ManakaCore::BoardView& board, Accumulator& acc, float* pre, std::string* err) override {
		// 全計算。直近の局面は使わない (検査が全計算と差分を別々に確かめられるように、ここは独立にしておく)。
		if (!net_->rebuild(board, acc.q16, err))
			return false;
		return evaluate(board.side_to_move, acc, pre, err);
	}

	bool rebuild_near(const ManakaCore::BoardView& board, Accumulator& acc, float* pre, std::string* err) override {
		ManakaCore::Q16Tokens tokens;
		if (!net_->make_tokens(board, tokens, err))
			return false;
		// トークンの差が最も小さい直近の局面を探す。差の大きさは両方向で同じ (どちらも 40 個)。
		int best = -1, best_diff = kMaxDiff + 1;
		for (int i = 0; i < ring_count_; ++i) {
			const int d = ManakaCore::q16_token_diff(tokens.bits[0], ring_[i].bits);
			if (d < best_diff) { best_diff = d; best = i; }
		}
		if (best >= 0) {
			if (!net_->update_from(tokens, ring_[best].acc, acc.q16, err))
				return false;
		} else if (!net_->rebuild(tokens, acc.q16)) {
			if (err) *err = "rebuild failed";
			return false;
		}
		remember(acc, tokens);
		return evaluate(board.side_to_move, acc, pre, err);
	}

	bool update(const ManakaCore::BoardView& board, const Accumulator& parent, Accumulator& acc,
	            float* pre, std::string* err) override {
		// update_from は out の全要素を書くので、parent を acc に写してから更新する必要は無い (parent には書かない)。
		ManakaCore::Q16Tokens tokens;
		if (!net_->make_tokens(board, tokens, err))
			return false;
		if (!net_->update_from(tokens, parent.q16, acc.q16, err))
			return false;
#if MANAKA_RING_REMEMBER_UPDATES
		remember(acc, tokens);
#endif
		return evaluate(board.side_to_move, acc, pre, err);
	}

	bool score(const ManakaCore::BoardView& board, const Accumulator& acc, float* pre, std::string* err) override {
		return evaluate(board.side_to_move, acc, pre, err);
	}

private:
	// 差 (入る駒と出る駒の数は同じ d 個) の差分更新が読む pair の行数は 79d - d^2、全計算は 780 行。
	// d = 11 で 748、d = 12 で 804 なので、d が 11 を超えたら全計算のほうが安い。
	static constexpr int kMaxDiff = 11;
#ifndef MANAKA_RING_SIZE
#define MANAKA_RING_SIZE 4
#endif
	static constexpr int kRing = MANAKA_RING_SIZE;

	struct Entry {
		ManakaCore::Q16Accumulator acc;
		uint64_t                   bits[ManakaCore::kQ16TokenWords];
	};

	// 直近に評価した局面のアキュムレータを覚える (古いものから上書きする)。
	void remember(const Accumulator& acc, const ManakaCore::Q16Tokens& tokens) {
		if (kRing == 0)
			return;
		Entry& e = ring_[ring_head_];
		e.acc = acc.q16;
		std::memcpy(e.bits, tokens.bits[0], sizeof(e.bits));
		ring_head_ = (ring_head_ + 1) % kRing;
		if (ring_count_ < kRing)
			ring_count_++;
	}

	bool evaluate(unsigned side_to_move, const Accumulator& acc, float* pre, std::string* err) {
		if (!net_->evaluate_pre(acc.q16, side_to_move, pre, err))
			return false;
		// core の evaluate も有限性を検査するが、この翻訳単位は厳密にビルドされるので、境目でも確かめる。
		if (!std::isfinite(*pre))
		{
			if (err)
				*err = "non-finite pre";
			return false;
		}
		return true;
	}

	const ManakaCore::Q16Network* net_;
	Entry                         ring_[kRing > 0 ? kRing : 1];
	int                           ring_head_  = 0;
	int                           ring_count_ = 0;
};

class Q16NetworkHolder final : public Network {
public:
	const OutputMap& output_map() const override { return map_; }

	std::unique_ptr<Session> new_session() const override {
		return std::unique_ptr<Session>(new Q16Session(&net_));
	}

	std::string describe() const override {
		return std::string(net_.checksum_verified() ? "SYMq16c" : "SYMq16") + " (nn.bin, integer) "
		     + (net_.has_hidden2() ? "h512_h2256" : "h512") + ", pair16_d128_o3, checksum_verified="
		     + (net_.checksum_verified() ? "verified" : "unverified");
	}

	Verification verification() const override {
		return net_.checksum_verified() ? Verification::kVerified : Verification::kUnverified;
	}

	ManakaCore::Q16Network net_;
	OutputMap              map_; // SYMq16 は cp の定義 (clamp(pre) * 1512.173) を固定で持つ。0.9 は YaneuraOu の内部単位への換算
};

} // namespace

std::unique_ptr<Network> load_network_q16(const std::string& path, bool require_checksum, std::string* err) {
	std::unique_ptr<Q16NetworkHolder> holder(new Q16NetworkHolder());
	// SYMq16c は CRC-32C を全バイトについて検証して読む。旧型の SYMq16 は require_checksum が偽なら未検証で受け入れる。
	if (!holder->net_.load(path, err, require_checksum))
		return nullptr;
	return holder;
}

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)
