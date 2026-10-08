#ifndef HCE_MODEL_H_INCLUDED
#define HCE_MODEL_H_INCLUDED

#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace YaneuraOu {
namespace Eval {
namespace Hce {

// explain が読む、局面を「駒得・速度・玉の安全度・駒の働き・手番」に分けるモデルのファイル。
//
// 形式は HCEXPL01 (scripts/export_hce_explain.py が書く)。
// 16 バイトの見出し (HCEXPL01、版、節の数) のあとに節が並び、節は
// 名前 8 バイト、パラメータ 3 つ、値の数、値 (float32) でできている。
// 知らない名前の節は読み飛ばすので、あとから節を足しても古い読み手は壊れない。
class Model {
public:
	struct Section {
		uint32_t           p[3] = { 0, 0, 0 };
		std::vector<float> v;
	};

	// 読めなかったときは false を返し、error に理由を入れる。
	bool load(const std::string& path, std::string& error);
	bool loaded() const { return loaded_; }

	// 名前の節。無ければ nullptr。
	const Section* find(const char* name) const;

	// この曲線がどの進行度のネットワークに合わせて学習されたか (hce_progress の fingerprint)。
	uint64_t progress_fingerprint() const { return fingerprint_; }

	// 進行度 (終局までの残り手数) での 5 つの要素の倍率。
	// 並びは 駒得 速度 玉の安全度 駒の働き 手番。
	static constexpr int NUM_FACTORS = 5;
	enum Factor { MATERIAL, SPEED, KING, ACTIVITY, TEMPO };
	float curve(int factor, float moves_left) const;

	// 駒の価値 (内部単位)。盤上 10 + 持ち駒 7。先手の枚数 - 後手の枚数に掛ける。
	static constexpr int NUM_MATERIAL = 17;
	const float* material_values() const { return material_; }

	// 手番の値。先手番なら +、後手番なら −。
	float tempo_value() const { return tempo_; }

private:
	bool                       loaded_ = false;
	uint64_t                   fingerprint_ = 0;
	std::map<std::string, Section> sections_;
	float                      knots_[5] = {};
	float                      curves_[NUM_FACTORS][5] = {};
	float                      material_[NUM_MATERIAL] = {};
	float                      tempo_ = 0.0f;
};

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
#endif // HCE_MODEL_H_INCLUDED
