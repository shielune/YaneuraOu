#ifndef HCE_PROGRESS_H_INCLUDED
#define HCE_PROGRESS_H_INCLUDED

#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include <cstdint>
#include <string>
#include <vector>

#include "../../types.h"

namespace YaneuraOu {

class Position;

namespace Eval {
namespace Hce {

// explain の進行度。「終局まであと何手か」を盤と持ち駒だけから当てる小さなネットワーク。
// どの NNUE 型でも使えるよう、評価関数の progress.bin には頼らない。
//
// 入力は、手番側から見た「どの駒がどの升にあるか」(28 種 x 81 升) と、両者の持ち駒。
// 出力は残り手数で、平均は 36 手ほど。局面の段階を決めるのに使う。
//
// ファイルは HCEPRG01 (scripts/export_hce_progress.py が書く)。読めなければ
// loaded() が false のままで、explain は進行度なしで動く。
class Progress {
public:
	// 読めなかったときは false を返し、error に理由を入れる。
	bool load(const std::string& path, std::string& error);
	bool loaded() const { return hidden_ != 0; }

	// 残り手数の推定。下限は 1。loaded() でなければ 0。
	float moves_left(const Position& pos) const;

	// 設定した重みを区別するための識別子 (ファイル全体の FNV-1a)。
	// 倍率の曲線がこの進行度に合わせて学習したものかを確かめるのに使う。
	uint64_t fingerprint() const { return fingerprint_; }

private:
	int hidden_ = 0;
	int second_ = 0;
	float scale_ = 0.0f;
	uint64_t fingerprint_ = 0;
	// 升と駒の表: 28 x 81 行、各 hidden_ 個。
	std::vector<float> ps_;
	// 持ち駒の層: 14 行 (入力) x hidden_ 個。偏りは hand_b_。
	std::vector<float> hand_w_, hand_b_;
	// 第二層: second_ 行 x hidden_ 個。
	std::vector<float> l1_w_, l1_b_;
	std::vector<float> out_w_;
	float out_b_ = 0.0f;
};

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
#endif // HCE_PROGRESS_H_INCLUDED
