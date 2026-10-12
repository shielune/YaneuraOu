#include "hce_model.h"

#if defined(USE_HCE_EXPLAIN)

#include <algorithm>
#include <cstring>
#include <fstream>

namespace YaneuraOu {
namespace Eval {
namespace Hce {

namespace {

constexpr char kMagic[8] = { 'H', 'C', 'E', 'X', 'P', 'L', '0', '1' };
constexpr size_t kHeaderBytes  = 16;
constexpr size_t kSectionBytes = 8 + 3 * 4 + 4;
// 1 つの節の値の数の上限。最大でも 玉の窓の第一層 (1575 x 幅) ほどで、これを超えれば壊れている。
constexpr uint32_t kMaxValues = 1u << 24;

} // namespace

bool Model::load(const std::string& path, std::string& error) {
	auto bad = [&](const std::string& why) { error = why; return false; };
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) return bad("failed to open " + path);
	const std::streamoff length = file.tellg();
	if (length < std::streamoff(kHeaderBytes)) return bad("file is shorter than its header");
	const size_t size = size_t(length);
	std::vector<unsigned char> bytes(size);
	file.seekg(0);
	if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)))
		return bad("read failed");
	if (std::memcmp(bytes.data(), kMagic, 8)) return bad("wrong magic");

	auto word = [&](size_t at) {
		const unsigned char* b = bytes.data() + at;
		return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
	};
	if (word(8) != 1) return bad("unsupported version");
	const uint32_t count = word(12);

	std::map<std::string, Section> sections;
	size_t at = kHeaderBytes;
	for (uint32_t s = 0; s < count; ++s) {
		if (at + kSectionBytes > size) return bad("a section header is cut off");
		char name[9] = {};
		std::memcpy(name, bytes.data() + at, 8);
		Section sec;
		for (int i = 0; i < 3; ++i) sec.p[i] = word(at + 8 + 4 * i);
		const uint32_t n = word(at + 20);
		at += kSectionBytes;
		if (n > kMaxValues || at + 4 * size_t(n) > size) return bad("a section is longer than the file");
		sec.v.resize(n);
		for (uint32_t i = 0; i < n; ++i) {
			const uint32_t bits = word(at + 4 * size_t(i));
			// -ffast-math の下でも非有限値を弾けるよう、ビットで見る。
			if ((bits & 0x7f800000u) == 0x7f800000u) return bad("nonfinite weight");
			std::memcpy(&sec.v[i], &bits, 4);
		}
		at += 4 * size_t(n);
		if (!sections.emplace(name, std::move(sec)).second) return bad(std::string("duplicate section ") + name);
	}
	if (at != size) return bad("trailing bytes after the last section");

	auto need = [&](const char* name, size_t n) -> const Section* {
		const auto it = sections.find(name);
		if (it == sections.end() || it->second.v.size() != n) { error = std::string("section ") + name + " is missing or has the wrong size"; return nullptr; }
		return &it->second;
	};
	const Section* progress = need("progress", 0);
	const Section* knots    = need("knots", 5);
	const Section* curves   = need("curves", NUM_FACTORS * 5);
	const Section* material = need("material", NUM_MATERIAL);
	const Section* tempo    = need("tempo", 1);
	if (!progress || !knots || !curves || !material || !tempo) return false;
	if (curves->p[0] != uint32_t(NUM_FACTORS) || curves->p[1] != 5) return bad("curves is not 5 factors x 5 knots");
	for (int i = 1; i < 5; ++i)
		if (!(knots->v[i] > knots->v[i - 1])) return bad("knots are not ascending");

	sections_    = std::move(sections);
	fingerprint_ = uint64_t(progress->p[0]) | (uint64_t(progress->p[1]) << 32);
	for (int i = 0; i < 5; ++i) knots_[i] = knots->v[i];
	for (int f = 0; f < NUM_FACTORS; ++f)
		for (int k = 0; k < 5; ++k) curves_[f][k] = curves->v[size_t(f) * 5 + k];
	for (int i = 0; i < NUM_MATERIAL; ++i) material_[i] = material->v[i];
	tempo_  = tempo->v[0];
	loaded_ = true;
	return true;
}

const Model::Section* Model::find(const char* name) const {
	const auto it = sections_.find(name);
	return it == sections_.end() ? nullptr : &it->second;
}

float Model::curve(int factor, float moves_left) const {
	// 区分線形: 隣り合う 2 つの点で重みを分け合い、端の外では端の点が全部持つ。
	const float p = std::min(std::max(moves_left, knots_[0]), knots_[4]);
	int hi = 1;
	while (hi < 4 && knots_[hi] <= p) ++hi;
	const int   lo   = hi - 1;
	const float frac = (p - knots_[lo]) / (knots_[hi] - knots_[lo]);
	return (1.0f - frac) * curves_[factor][lo] + frac * curves_[factor][hi];
}

} // namespace Hce
} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
