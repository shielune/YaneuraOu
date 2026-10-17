#ifndef Q16_MKC_H_INCLUDED
#define Q16_MKC_H_INCLUDED
// Test fixture writer: turns a ModelType=SYMq16 nn.bin into ModelType=SYMq16c (same body, new header string and
// section ids, CRC-32C trailer). Independent of the Rust exporter so the two writers can be compared byte for byte.
#include "q16.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace q16mkc {

inline uint32_t fnv1a(const uint8_t* p, size_t n) {
	uint32_t h = 0x811C9DC5u;
	for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
	return h;
}
inline uint32_t read_u32(const std::vector<uint8_t>& v, size_t at) { uint32_t x; std::memcpy(&x, v.data() + at, 4); return x; }
inline void put_u32(std::vector<uint8_t>& v, uint32_t x) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
	v.insert(v.end(), p, p + 4);
}

inline bool make_checksummed(const std::string& in_path, const std::string& out_path, std::string* error) {
	std::ifstream in(in_path, std::ios::binary);
	std::vector<uint8_t> src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto fail = [&](const char* m) { if (error) *error = m; return false; };
	if (src.size() < 12 || read_u32(src, 0) != 0x7AF32F16u) return fail("not a SYMq16 file");
	const uint32_t len = read_u32(src, 8), old_hash = read_u32(src, 4);
	if (src.size() < 12 + size_t(len)) return fail("short header");
	const std::string arch(reinterpret_cast<const char*>(src.data() + 12), len);
	const std::string marker = "ModelType=SYMq16;";
	if (arch.compare(0, marker.size(), marker) != 0) return fail("ModelType is not SYMq16");
	const bool h2 = arch.find("Dense[1<-256]") != std::string::npos;
	std::string new_arch = arch;
	new_arch.insert(marker.size() - 1, "c"); // ModelType=SYMq16c;
	const uint32_t hash = fnv1a(reinterpret_cast<const uint8_t*>(new_arch.data()), new_arch.size());
	// Body sizes in bytes of the sections that follow the header (1..6; the h2 section exists only for h2 shapes).
	std::vector<size_t> sizes = {2344u * 128 * 2, 2745996u * 16 * 2, 400 * 4, 512u * 800 * 2 + 512 * 4 * 2};
	if (h2) sizes.push_back(512 * 4 + 256u * 512 * 2 + 256 * 4 * 2);
	sizes.push_back((h2 ? 256u : 512u) * 4 + 4);
	std::vector<uint8_t> out;
	out.reserve(src.size() + 16);
	put_u32(out, 0x7AF32F16u);
	put_u32(out, hash);
	put_u32(out, static_cast<uint32_t>(new_arch.size()));
	out.insert(out.end(), new_arch.begin(), new_arch.end());
	size_t at = 12 + len;
	unsigned section = 0;
	for (size_t bytes : sizes) {
		++section;
		if (section == 5 && !h2) section = 6;
		if (at + 4 + bytes > src.size() || read_u32(src, at) != old_hash + section) return fail("unexpected section layout");
		put_u32(out, hash + section);
		out.insert(out.end(), src.begin() + at + 4, src.begin() + at + 4 + bytes);
		at += 4 + bytes;
	}
	if (at != src.size()) return fail("trailing bytes in the SYMq16 source");
	// The software CRC on purpose: the loader uses the hardware path, so the two get compared.
	const uint32_t crc = YaneuraOu::Eval::ManakaCore::q16_crc32c(out.data(), out.size(), false);
	put_u32(out, hash + 7);
	put_u32(out, crc);
	std::ofstream o(out_path, std::ios::binary | std::ios::trunc);
	o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
	return bool(o);
}

} // namespace q16mkc
#endif
