#ifndef MANAKA_CORE_EVAL_F32_H_INCLUDED
#define MANAKA_CORE_EVAL_F32_H_INCLUDED

// Manaka の SYM 評価を、アキュムレータの差分更新なしに、局面ごとに一から f32 で計算する。
//
// 正本は Rust 側の crates/manaka-core/src/eval.rs と accum/{sums,newton}.rs。計算の順序を Rust と揃えてあるので、
// 同じ重みと局面なら value と pre はビット単位で一致することを狙っている(ビルドの条件は下)。
//
//   1. 各枠のトークン id から、チャネルごとの冪和 p_k = Σ embed[id][c]^k (k = 1..order)
//      を、トークンの並びの順に f32 で足す。冪は v、v*v、(v*v)*v、...。
//   2. Newton の恒等式で初等対称式 e_k を作る(acc に (符号 * e_{k-j}) * p_j を j = 1..k の順に足し、最後に k で割る)。
//   3. 各 e_k を RMS で正規化して gamma を掛ける: (e * (1 / sqrt(ms + 1e-6))) * gamma。ms は Σ e*e を先頭から順に足して dim で割る。
//   4. pair_dim > 0 なら、その枠の全対 {a, b} の pair 表の行を足す。足す順は Rust のアキュムレータの rebuild と同じ
//      (トークンを並びの順に加え、各トークンについてそれより前のトークンとの対を前から)。mean_dim = pair_dim の形で、
//      冪和と同じ式で RMS 正規化して pair_gamma を掛ける。
//   5. 特徴 = [手番側の枠のブロック, 相手側の枠のブロック]。1 枠のブロックは [e_1 ... e_order, pair]。
//   6. h = max(0, b1 + W1 x)。hidden2 があれば h = max(0, b2 + W2 h)。
//   7. pre = value_b + value_w · h、value = tanh(pre)。
//
// W1 と W2 と value 行の内積は、8 レーンに分けて足し、最後にレーン 0 から順に足し、余りは最後に 1 つずつ足す。
// この順序が Rust の eval.rs の dot と同じで、変えると f32 の答えが変わる。
//
// ビルドの条件: -ffast-math を使わない。-ffp-contract=off にする(乗算と加算を FMA に融合すると別の f32 になる)。
// -mavx2 があれば AVX2 の経路、なければ同じ順序のスカラーの経路を使う。どちらも結果は同じ。
//
// 対象の形(v1): Tokens 空間の pair と h2 と order 1 から 4 と任意の dim と hidden。lse、ks、m、f、split、nnue、
// KP と HalfKP は create() が拒否する(MnwWeights::load の時点で拒否済み)。

#include <cstdint>
#include <memory>
#include <string>

#include "board.h"
#include "common.h"
#include "mnw.h"
#include "tokens.h"

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

// 評価の段。EvalTrace に段ごとの時間を足す。
enum EvalStage {
	kStageTokens,  // BoardView からトークン id を作る
	kStagePowers,  // 冪和(2 枠分)
	kStageNewton,  // Newton と、各次数の RMS 正規化
	kStagePair,    // pair の行の添字作り、行の和、RMS 正規化
	kStageW1,      // W1 と ReLU
	kStageW2,      // W2 と ReLU(hidden2 がなければ 0)
	kStageOut,     // value 行と tanh
	kNumEvalStages
};

// rdtsc の刻みを段ごとに足していく。呼び出し側がゼロにして渡す。nullptr なら計らない。
struct EvalTrace {
	uint64_t ticks[kNumEvalStages];
	void clear() { for (uint64_t& t : ticks) t = 0; }
};

class EvaluatorF32 {
public:
	struct Result {
		float value; // tanh の後。手番側から見た値
		float pre;   // tanh の前。手番側から見た値
	};

	// weights は評価器より長く生きていること。同じオブジェクトを再ロードまたは move した場合は、この評価器を作り直す。
	// 再ロードと評価を並行させないこと。古い評価器の使用は版の照合で拒否する。作業用の領域は評価器ごとに持つので、スレッドごとに 1 つ作る。
	static std::unique_ptr<EvaluatorF32> create(const MnwWeights* weights, std::string* err);

	// board を評価する。board が範囲外(validate_board が false)なら false を返し、*r は変えない。
	bool evaluate(const BoardView& board, Result* r, EvalTrace* trace = nullptr);

	// 特徴ベクトル(feature_len 個)を out に書く。評価の前半だけで、W1 以降は走らない。
	bool build_features(const BoardView& board, float* out);

	// 特徴ベクトルから W1 以降を走らせて pre を返す(value は呼び出し側が tanh する)。
	// 重みの版が変わっていたら配列を読まず NaN を返す。
	float forward(const float* features);

	const MnwHeader& header() const { return weights_->header(); }

private:
	EvaluatorF32() = default;

	bool build_features_impl(const BoardView& board, float* feat, EvalTrace* trace);
	float forward_impl(const float* feat, EvalTrace* trace);

	const MnwWeights* weights_ = nullptr;
	uint64_t weights_revision_ = 0;
	size_t dim_ = 0, order_ = 0, pair_dim_ = 0, hidden_n_ = 0, hidden2_n_ = 0, block_ = 0;

	AlignedBuffer<float> p_;       // [order][dim] 冪和
	AlignedBuffer<float> e_;       // [order + 1][dim] 初等対称式。e_[0] は 1
	AlignedBuffer<float> acc_;     // [dim]
	AlignedBuffer<float> q_;       // [pair_dim]
	AlignedBuffer<uint32_t> rows_; // pair の行番号
	AlignedBuffer<float> feat_;    // [feature_len]
	AlignedBuffer<float> hidden_;  // [hidden]
	AlignedBuffer<float> head_;    // [hidden2]
};

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu

#endif // MANAKA_CORE_EVAL_F32_H_INCLUDED
