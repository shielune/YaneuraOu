#include "eval_f32.h"

#include <cmath>
#include <cstring>
#include <limits>

#if defined(__SSE__)
#include <immintrin.h>
#endif

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

namespace {

constexpr float kRmsEps = 1e-6f;

// pair の行を何行先まで先読みするか。行の添字は評価の前に全部決まるので、メモリの待ちを重ねられる。
constexpr int kPairPrefetchDistance = 16;

inline uint64_t now_ticks() {
#if defined(__x86_64__) || defined(__i386__)
	return __builtin_ia32_rdtsc();
#else
	return 0;
#endif
}

// trace が nullptr のとき何もしない、段の計時。
struct Lap {
	EvalTrace* trace;
	uint64_t last;
	explicit Lap(EvalTrace* t) : trace(t), last(t ? now_ticks() : 0) {}
	void mark(EvalStage stage) {
		if (trace) {
			const uint64_t now = now_ticks();
			trace->ticks[stage] += now - last;
			last = now;
		}
	}
};

// --- 冪和

// p[k][c] += v^k を、トークンの並びの順に足す。冪は v、v*v、(v*v)*v、...(Rust の touch_powers と同じ)。
template<int ORDER>
void accumulate_powers(const float* __restrict embed, size_t dim, const uint16_t* ids, int n, float* __restrict p) {
	float* p1 = p;
	float* p2 = ORDER >= 2 ? p + dim : p;
	float* p3 = ORDER >= 3 ? p + 2 * dim : p;
	float* p4 = ORDER >= 4 ? p + 3 * dim : p;
	for (int t = 0; t < n; ++t) {
		const float* row = embed + size_t(ids[t]) * dim;
		for (size_t c = 0; c < dim; ++c) {
			const float v = row[c];
			float power = v;
			p1[c] += power;
			if (ORDER >= 2) {
				power *= v;
				p2[c] += power;
			}
			if (ORDER >= 3) {
				power *= v;
				p3[c] += power;
			}
			if (ORDER >= 4) {
				power *= v;
				p4[c] += power;
			}
		}
	}
}

// --- Newton の恒等式
//
// k * e_k = Σ_{j=1..k} (-1)^(j-1) * e_(k-j) * p_j。e[0] 行は 1。
// 1 チャネルの中の演算の順序は Rust の elementary_from_power と同じ: acc = 0、j = 1..k の順に acc += (sign * e[k-j]) * p[j]、最後に acc / k。
void newton(const float* __restrict p, int order, size_t dim, float* __restrict e, float* __restrict acc) {
	for (int k = 1; k <= order; ++k) {
		for (size_t c = 0; c < dim; ++c)
			acc[c] = 0.0f;
		float sign = 1.0f;
		for (int j = 1; j <= k; ++j) {
			const float* ek = e + size_t(k - j) * dim;
			const float* pj = p + size_t(j - 1) * dim;
			for (size_t c = 0; c < dim; ++c)
				acc[c] += sign * ek[c] * pj[c];
			sign = -sign;
		}
		float* out = e + size_t(k) * dim;
		const float fk = float(k);
		for (size_t c = 0; c < dim; ++c)
			out[c] = acc[c] / fk;
	}
}

// --- RMS 正規化

// scale = 1 / sqrt(Σ v*v / n + eps)。Σ は先頭から順に足す(順序が結果を変える)。
inline float rms_scale(const float* v, size_t n) {
	float sum = 0.0f;
	for (size_t i = 0; i < n; ++i)
		sum += v[i] * v[i];
	const float mean_square = sum / float(n);
	const float rms = std::sqrt(mean_square + kRmsEps);
	return 1.0f / rms;
}

// out[c] = (v[c] * scale) * gamma[c]。scale * gamma を先にまとめると別の f32 になる。
inline void scale_gamma(const float* v, float scale, const float* gamma, size_t n, float* out) {
	for (size_t c = 0; c < n; ++c)
		out[c] = v[c] * scale * gamma[c];
}

// --- pair の行の和

inline void prefetch_row(const float* row, size_t row_bytes) {
#if defined(__SSE__)
	const char* p = reinterpret_cast<const char*>(row);
	for (size_t off = 0; off < row_bytes; off += 64)
		_mm_prefetch(p + off, _MM_HINT_T0);
	_mm_prefetch(p + row_bytes - 1, _MM_HINT_T0);
#else
	(void)row;
	(void)row_bytes;
#endif
}

// q[c] = Σ_i pair[rows[i]][c]。行 i = 0, 1, ... の順に、チャネルごとに f32 で足す。
void pair_sum_generic(const float* pair, const uint32_t* rows, int m, size_t pd, float* q) {
	for (size_t c = 0; c < pd; ++c)
		q[c] = 0.0f;
	const size_t row_bytes = pd * 4;
	for (int i = 0; i < m; ++i) {
		if (i + kPairPrefetchDistance < m)
			prefetch_row(pair + size_t(rows[i + kPairPrefetchDistance]) * pd, row_bytes);
		const float* row = pair + size_t(rows[i]) * pd;
		for (size_t c = 0; c < pd; ++c)
			q[c] += row[c];
	}
}

#if defined(__AVX2__)
// pd が 8 の NB 倍のとき。アキュムレータをレジスタに置いたまま全行を足す。
template<int NB>
void pair_sum_avx2(const float* pair, const uint32_t* rows, int m, float* q) {
	constexpr size_t pd = 8 * NB;
	constexpr size_t row_bytes = pd * 4;
	__m256 acc[NB];
	for (int b = 0; b < NB; ++b)
		acc[b] = _mm256_setzero_ps();
	for (int i = 0; i < kPairPrefetchDistance && i < m; ++i)
		prefetch_row(pair + size_t(rows[i]) * pd, row_bytes);
	for (int i = 0; i < m; ++i) {
		if (i + kPairPrefetchDistance < m)
			prefetch_row(pair + size_t(rows[i + kPairPrefetchDistance]) * pd, row_bytes);
		const float* row = pair + size_t(rows[i]) * pd;
		for (int b = 0; b < NB; ++b)
			acc[b] = _mm256_add_ps(acc[b], _mm256_loadu_ps(row + 8 * b));
	}
	for (int b = 0; b < NB; ++b)
		_mm256_storeu_ps(q + 8 * b, acc[b]);
}
#endif

void pair_sum(const float* pair, const uint32_t* rows, int m, size_t pd, float* q) {
#if defined(__AVX2__)
	switch (pd) {
	case 8:  pair_sum_avx2<1>(pair, rows, m, q); return;
	case 16: pair_sum_avx2<2>(pair, rows, m, q); return;
	case 24: pair_sum_avx2<3>(pair, rows, m, q); return;
	case 32: pair_sum_avx2<4>(pair, rows, m, q); return;
	case 40: pair_sum_avx2<5>(pair, rows, m, q); return;
	case 48: pair_sum_avx2<6>(pair, rows, m, q); return;
	case 56: pair_sum_avx2<7>(pair, rows, m, q); return;
	case 64: pair_sum_avx2<8>(pair, rows, m, q); return;
	default: break;
	}
#endif
	pair_sum_generic(pair, rows, m, pd, q);
}

// --- 行列とベクトルの積

// 8 レーンのアキュムレータの合計を、レーン 0 から順に足し、余りを 1 つずつ足す(Rust の dot の最後の段)。
inline float finish_lanes(const float* lanes, const float* w, const float* x, size_t tail_begin, size_t n) {
	float total = 0.0f;
	for (int l = 0; l < 8; ++l)
		total += lanes[l];
	for (size_t i = tail_begin; i < n; ++i)
		total += w[i] * x[i];
	return total;
}

// out[r] = max(0, bias[r] + dot(w[r], x))。w は [rows][cols] の行優先。
// 1 行ごとの演算は Rust の dot そのもの(8 レーン、乗算と加算は別、最後にレーン 0 から順)。
// AVX2 の経路は 4 行を同時に走らせるが、各行の演算の順序は変わらない(加算の遅延を隠すだけ)。
void matvec_relu(const float* w, size_t rows, size_t cols, const float* x, const float* bias, float* out) {
	const size_t chunks = cols / 8;
	const size_t tail_begin = chunks * 8;
	size_t r = 0;
#if defined(__AVX2__)
	alignas(32) float lanes[4][8];
	for (; r + 4 <= rows; r += 4) {
		const float* w0 = w + r * cols;
		const float* w1 = w0 + cols;
		const float* w2 = w1 + cols;
		const float* w3 = w2 + cols;
		__m256 a0 = _mm256_setzero_ps();
		__m256 a1 = _mm256_setzero_ps();
		__m256 a2 = _mm256_setzero_ps();
		__m256 a3 = _mm256_setzero_ps();
		for (size_t c = 0; c < chunks; ++c) {
			const __m256 xv = _mm256_loadu_ps(x + 8 * c);
			a0 = _mm256_add_ps(a0, _mm256_mul_ps(_mm256_loadu_ps(w0 + 8 * c), xv));
			a1 = _mm256_add_ps(a1, _mm256_mul_ps(_mm256_loadu_ps(w1 + 8 * c), xv));
			a2 = _mm256_add_ps(a2, _mm256_mul_ps(_mm256_loadu_ps(w2 + 8 * c), xv));
			a3 = _mm256_add_ps(a3, _mm256_mul_ps(_mm256_loadu_ps(w3 + 8 * c), xv));
		}
		_mm256_store_ps(lanes[0], a0);
		_mm256_store_ps(lanes[1], a1);
		_mm256_store_ps(lanes[2], a2);
		_mm256_store_ps(lanes[3], a3);
		const float* ws[4] = { w0, w1, w2, w3 };
		for (int i = 0; i < 4; ++i) {
			const float v = bias[r + i] + finish_lanes(lanes[i], ws[i], x, tail_begin, cols);
			out[r + i] = v > 0.0f ? v : 0.0f;
		}
	}
	for (; r < rows; ++r) {
		const float* wr = w + r * cols;
		__m256 a = _mm256_setzero_ps();
		for (size_t c = 0; c < chunks; ++c)
			a = _mm256_add_ps(a, _mm256_mul_ps(_mm256_loadu_ps(wr + 8 * c), _mm256_loadu_ps(x + 8 * c)));
		_mm256_store_ps(lanes[0], a);
		const float v = bias[r] + finish_lanes(lanes[0], wr, x, tail_begin, cols);
		out[r] = v > 0.0f ? v : 0.0f;
	}
#else
	for (; r < rows; ++r) {
		const float* wr = w + r * cols;
		float lanes[8] = {};
		for (size_t c = 0; c < chunks; ++c)
			for (int l = 0; l < 8; ++l)
				lanes[l] += wr[8 * c + l] * x[8 * c + l];
		const float v = bias[r] + finish_lanes(lanes, wr, x, tail_begin, cols);
		out[r] = v > 0.0f ? v : 0.0f;
	}
#endif
}

// 1 本の内積(value 行)。
float dot8(const float* a, const float* b, size_t n) {
	const size_t chunks = n / 8;
	alignas(32) float lanes[8] = {};
#if defined(__AVX2__)
	__m256 acc = _mm256_setzero_ps();
	for (size_t c = 0; c < chunks; ++c)
		acc = _mm256_add_ps(acc, _mm256_mul_ps(_mm256_loadu_ps(a + 8 * c), _mm256_loadu_ps(b + 8 * c)));
	_mm256_store_ps(lanes, acc);
#else
	for (size_t c = 0; c < chunks; ++c)
		for (int l = 0; l < 8; ++l)
			lanes[l] += a[8 * c + l] * b[8 * c + l];
#endif
	return finish_lanes(lanes, a, b, chunks * 8, n);
}

} // namespace

std::unique_ptr<EvaluatorF32> EvaluatorF32::create(const MnwWeights* weights, std::string* err) {
	auto fail = [&](const std::string& m) -> std::unique_ptr<EvaluatorF32> {
		if (err)
			*err = m;
		return nullptr;
	};
	if (weights == nullptr)
		return fail("weights が nullptr");
	const MnwHeader& h = weights->header();
	if (const char* why = mnw_unsupported_reason(h))
		return fail(std::string("対象外の形 ") + mnw_shape_name(h) + ": " + why);
	if (h.version != kMnwVersion || h.order < 1 || h.order > kMaxOrder || h.dim == 0 || h.hidden == 0)
		return fail("MnwWeights が初期化されていない");

	std::unique_ptr<EvaluatorF32> ev(new EvaluatorF32());
	ev->weights_ = weights;
	ev->weights_revision_ = weights->revision();
	ev->dim_ = h.dim;
	ev->order_ = h.order;
	ev->pair_dim_ = h.pair_dim;
	ev->hidden_n_ = h.hidden;
	ev->hidden2_n_ = h.hidden2;
	ev->block_ = h.block_len;

	const size_t max_pairs = size_t(kMaxTokensPerFrame) * (kMaxTokensPerFrame - 1) / 2;
	bool ok = ev->p_.allocate(ev->order_ * ev->dim_) && ev->e_.allocate((ev->order_ + 1) * ev->dim_) && ev->acc_.allocate(ev->dim_) &&
	          ev->q_.allocate(ev->pair_dim_) && ev->rows_.allocate(ev->pair_dim_ ? max_pairs : 0) && ev->feat_.allocate(h.feature_len) &&
	          ev->hidden_.allocate(ev->hidden_n_) && ev->head_.allocate(ev->hidden2_n_);
	if (!ok)
		return fail("作業用のメモリを確保できない");
	for (size_t c = 0; c < ev->dim_; ++c)
		ev->e_[c] = 1.0f; // e_0 = 1
	return ev;
}

bool EvaluatorF32::build_features_impl(const BoardView& board, float* feat, EvalTrace* trace) {
	if (weights_->revision() != weights_revision_)
		return false;
	Lap lap(trace);
	uint16_t ids[2][kMaxTokensPerFrame];
	if (board.side_to_move > 1)
		return false;
	const int n = collect_tokens(board, ids);
	if (n < 0)
		return false;
	lap.mark(kStageTokens);

	const float* embed = weights_->embed();
	const float* gamma = weights_->gamma();
	const size_t dim = dim_;
	const int order = int(order_);
	const int stm = board.side_to_move;

	for (int half = 0; half < 2; ++half) {
		// 先に手番側の枠、次に相手側の枠。
		const int frame = half == 0 ? stm : 1 - stm;
		const uint16_t* fids = ids[frame];
		float* out = feat + size_t(half) * block_;

		std::memset(p_.data(), 0, order_ * dim * sizeof(float));
		switch (order) {
		case 1: accumulate_powers<1>(embed, dim, fids, n, p_.data()); break;
		case 2: accumulate_powers<2>(embed, dim, fids, n, p_.data()); break;
		case 3: accumulate_powers<3>(embed, dim, fids, n, p_.data()); break;
		default: accumulate_powers<4>(embed, dim, fids, n, p_.data()); break;
		}
		lap.mark(kStagePowers);

		newton(p_.data(), order, dim, e_.data(), acc_.data());
		for (int k = 0; k < order; ++k) {
			const float* e = e_.data() + size_t(k + 1) * dim;
			const float scale = rms_scale(e, dim);
			scale_gamma(e, scale, gamma + size_t(k) * dim, dim, out + size_t(k) * dim);
		}
		lap.mark(kStageNewton);

		if (pair_dim_ > 0) {
			// 行の添字: トークンを並びの順に加えるとして、各トークンについて、それより前のトークンとの対を前から。
			uint32_t* rows = rows_.data();
			int m = 0;
			for (int t = 1; t < n; ++t)
				for (int j = 0; j < t; ++j)
					rows[m++] = pair_index(fids[t], fids[j]);
			pair_sum(weights_->pair(), rows, m, pair_dim_, q_.data());
			// mean_dim = pair_dim - pair_lse。対象の形は pair_lse = 0 なので全チャネルが平均側。
			const float scale = rms_scale(q_.data(), pair_dim_);
			scale_gamma(q_.data(), scale, weights_->pair_gamma(), pair_dim_, out + size_t(order) * dim);
		}
		lap.mark(kStagePair);
	}
	return true;
}

float EvaluatorF32::forward_impl(const float* feat, EvalTrace* trace) {
	Lap lap(trace);
	const MnwHeader& h = weights_->header();
	matvec_relu(weights_->w1(), hidden_n_, size_t(h.feature_len), feat, weights_->b1(), hidden_.data());
	lap.mark(kStageW1);

	const float* head = hidden_.data();
	size_t head_n = hidden_n_;
	if (hidden2_n_ > 0) {
		matvec_relu(weights_->w2(), hidden2_n_, hidden_n_, hidden_.data(), weights_->b2(), head_.data());
		head = head_.data();
		head_n = hidden2_n_;
	}
	lap.mark(kStageW2);

	const float pre = weights_->value_b() + dot8(weights_->value_w(), head, head_n);
	lap.mark(kStageOut);
	return pre;
}

bool EvaluatorF32::build_features(const BoardView& board, float* out) { return build_features_impl(board, out, nullptr); }

float EvaluatorF32::forward(const float* features) {
	if (weights_->revision() != weights_revision_)
		return std::numeric_limits<float>::quiet_NaN();
	return forward_impl(features, nullptr);
}

bool EvaluatorF32::evaluate(const BoardView& board, Result* r, EvalTrace* trace) {
	if (!build_features_impl(board, feat_.data(), trace))
		return false;
	const float pre = forward_impl(feat_.data(), trace);
	Lap lap(trace);
	r->pre = pre;
	r->value = ::tanhf(pre);
	lap.mark(kStageOut);
	return true;
}

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu
