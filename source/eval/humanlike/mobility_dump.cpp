// HCE v2 の feature dump。
//
// この file は Makefile の SOURCES に入っていない (source/Makefile は今回の担当範囲外)。
// そのため humanlike_eval.cpp の末尾から #include して同じ translation unit に
// 取り込んでいる。将来 Makefile に足すときは、humanlike_eval.cpp 側の #include を
// 消してから SOURCES に加え、HCE_DUMP_STANDALONE_TU を定義すること。
// (どちらの入口も定義されていないときは、この file は空になる)
#if defined(HCE_DUMP_INCLUDED_FROM_HUMANLIKE_EVAL) || defined(HCE_DUMP_STANDALONE_TU)

#if defined(HCE_DUMP_STANDALONE_TU)
#include "humanlike_eval.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "../../position.h"
#else
#include <algorithm>
#include <cstdint>
#include <iomanip>
#endif

namespace YaneuraOu {
namespace Eval {
namespace HumanLike {

namespace {

// 教師 binpack の 1 record。やねうら王の Learner::PackedSfenValue と同じ 40 byte。
// この fork には learn/ が無いので、ここで形だけ定義する。
struct HcePackedSfenValue {
	PackedSfen sfen;     // 32
	int16_t    score;    //  2  手番側から見た評価値
	uint16_t   move;     //  2
	uint16_t   gamePly;  //  2
	int8_t     game_result; // 1
	uint8_t    padding;  //  1
};
static_assert(sizeof(HcePackedSfenValue) == 40, "PackedSfenValue must be 40 bytes");

struct DumpArgs {
	std::string input;
	std::string output;          // 空なら標準出力 (テキスト dump) / 必須 (binpack dump)
	std::string output_prefix = "hce_features";
	uint64_t    max_positions = 0;   // 0 なら全部
	uint64_t    skip = 0;
	int         dims = 0;            // 0 なら版の v2 の幅
	bool        names = false;       // 列名も書き出す
	Layout      layout;              // 既定は plain
	int         kind = 1;            // 0 利きだけ / 1 v2 / 2 按分
};

// "input /path output /path variant rank+contact dims 1087" のような並びを読む。
// variant を書かずに dims だけ書いてもよい。六つの版の 18 通りの幅はすべて異なるので、
// dims だけで版と種類が決まる。
bool parse_dump_args(const std::string& s, DumpArgs& a, const char* who) {
	std::istringstream is(s);
	std::string token, variant, pieces;
	while (is >> token) {
		if      (token == "input")         is >> a.input;
		else if (token == "output")        is >> a.output;
		else if (token == "output_prefix") is >> a.output_prefix;
		else if (token == "max_positions") is >> a.max_positions;
		else if (token == "max")           is >> a.max_positions;
		else if (token == "skip")          is >> a.skip;
		else if (token == "dims")          is >> a.dims;
		else if (token == "variant")       is >> variant;
		else if (token == "pieces")        is >> pieces;
		else if (token == "names")         a.names = true;
		else {
			sync_cout << "info string " << who << ": unknown token: " << token << sync_endl;
			return false;
		}
	}

	if (!variant.empty() || !pieces.empty()) {
		Variant v = Variant::Plain;
		Pieces p = Pieces::Folded;
		if (!variant.empty() && !parse_variant(variant, v)) {
			sync_cout << "info string " << who << ": unknown variant: " << variant << sync_endl;
			return false;
		}
		if (!pieces.empty() && !parse_pieces(pieces, p)) {
			sync_cout << "info string " << who << ": unknown pieces: " << pieces << sync_endl;
			return false;
		}
		a.layout = make_layout(v, p);
		if (a.dims == 0) a.dims = a.layout.features();
		a.kind = (a.dims == a.layout.mobility)   ? 0
		       : (a.dims == a.layout.features()) ? 1
		       : (a.dims == a.layout.tapered())  ? 2
		                                         : -1;
		if (a.kind < 0) {
			sync_cout << "info string " << who << ": dims must be " << a.layout.mobility
			          << " / " << a.layout.features() << " / " << a.layout.tapered()
			          << " for variant " << variant_name(v) << sync_endl;
			return false;
		}
		return true;
	}

	if (a.dims == 0) a.dims = a.layout.features();
	if (!layout_for_dim(a.dims, a.layout, a.kind)) {
		sync_cout << "info string " << who << ": dims " << a.dims
		          << " is not a width any variant has" << sync_endl;
		return false;
	}
	return true;
}

void extract_dims(const Position& pos, const Layout& layout, int kind, float* feat) {
	if      (kind == 0) extract_features(pos, layout, feat);
	else if (kind == 1) extract_features_v2(pos, layout, feat);
	else                extract_features_phased(pos, layout, feat);
}

// 整数なら整数として、そうでなければ小数で書く。
// 玉の安全度のうち enemy_hand_times_exposure の 7 個だけが 1/8 の倍数になるので、
// ここを丸めてしまうと Rust 側との突き合わせが意味を失う。
void write_value(std::ostream& os, double v) {
	if (v == (double)(long long)v)
		os << (long long)v;
	else
		os << std::setprecision(9) << v;
}

void write_row(std::ostream& os, const char* key, const float* v, int n) {
	os << key;
	for (int i = 0; i < n; ++i) {
		os << ' ';
		write_value(os, (double)v[i]);
	}
	os << '\n';
}

} // namespace

// ---------------------------------------------------------------------------
// テキスト dump: Rust 側の `manaka-hce-fit dump` と 1 列ずつ突き合わせるための出力
// ---------------------------------------------------------------------------
// 1 局面につき次の行を書く。
//   sfen    <sfen>
//   mat     <17>            駒得       (先手視点)
//   want    <利き>          利き       (先手視点、plain なら 164 列)
//   king    <50>            玉の安全度 (先手視点)
//   featv2  <駒得+利き+玉>  上の三つを仕様書の順に並べたもの
//   phase   <t>             進行度
void hce_dump_position(std::ostream& os, const Position& pos, const Layout& layout,
                       const std::string& sfen) {
	float mat[MAX_MATERIAL_FEATURES];
	float kng[NUM_KING_FEATURES];
	std::vector<float> mob((size_t)layout.mobility);
	std::vector<float> v2((size_t)layout.features());

	extract_material_features(pos, layout, mat);
	extract_features(pos, layout, mob.data());
	extract_king_features(pos, kng);
	extract_features_v2(pos, layout, v2.data());

	os << "sfen " << sfen << '\n';
	write_row(os, "mat",     mat, layout.material);
	write_row(os, "want",    mob.data(), layout.mobility);
	write_row(os, "king",    kng, NUM_KING_FEATURES);
	write_row(os, "featv2",  v2.data(), layout.features());
	os << "phase ";
	write_value(os, (double)game_phase(pos));
	os << '\n';
}

void hce_dump_sfen(std::ostream& os, const Layout& layout, const std::string& sfen) {
	Position  pos;
	StateInfo si;
	auto err = pos.set(sfen, &si);
	if (err.has_value()) {
		os << "error bad sfen: " << sfen << '\n';
		return;
	}
	hce_dump_position(os, pos, layout, sfen);
}

// sfen の 1 行を切り出す。空行と # の行は空文字を返す。
// "sfen ..." / "position sfen ..." で始まっていても受ける。
static std::string strip_sfen_line(const std::string& line) {
	size_t b = line.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return std::string();
	size_t e = line.find_last_not_of(" \t\r\n");
	std::string sfen = line.substr(b, e - b + 1);
	if (sfen[0] == '#') return std::string();
	if (sfen.rfind("position ", 0) == 0) sfen = sfen.substr(9);
	if (sfen.rfind("sfen ", 0) == 0)     sfen = sfen.substr(5);
	return sfen;
}

// SFEN を 1 行 1 局面で読み、上の形式で書き出す。
// 使い方 (USI):  setoption name HceDumpFile value input /tmp/pos.sfen output /tmp/engine.txt
void hce_dump_file_cmd(const std::string& args) {
	DumpArgs a;
	if (!parse_dump_args(args, a, "HceDumpFile")) return;
	if (a.input.empty()) {
		sync_cout << "info string HceDumpFile: input is required" << sync_endl;
		return;
	}

	std::ifstream ifs(a.input);
	if (!ifs) {
		sync_cout << "info string HceDumpFile: failed to open " << a.input << sync_endl;
		return;
	}

	std::ofstream ofs;
	if (!a.output.empty()) {
		ofs.open(a.output);
		if (!ofs) {
			sync_cout << "info string HceDumpFile: failed to write " << a.output << sync_endl;
			return;
		}
	}
	std::ostream& os = a.output.empty() ? std::cout : ofs;

	if (a.names) {
		os << "names";
		for (int i = 0; i < a.layout.features(); ++i)
			os << ' ' << feature_v2_name(a.layout, i);
		os << '\n';
	}

	Position  pos;
	StateInfo si;
	uint64_t  seen = 0, done = 0, bad = 0;
	std::string line;
	while (std::getline(ifs, line)) {
		std::string sfen = strip_sfen_line(line);
		if (sfen.empty()) continue;

		if (seen++ < a.skip) continue;
		if (a.max_positions && done >= a.max_positions) break;

		auto err = pos.set(sfen, &si);
		if (err.has_value()) {
			os << "error bad sfen: " << sfen << '\n';
			++bad;
			continue;
		}
		hce_dump_position(os, pos, a.layout, sfen);
		++done;
	}
	os.flush();

	sync_cout << "info string HceDumpFile: wrote " << done << " positions"
	          << (bad ? (" (" + std::to_string(bad) + " bad)") : std::string())
	          << (a.output.empty() ? std::string() : (" to " + a.output)) << sync_endl;
}

// ---------------------------------------------------------------------------
// binpack dump: 学習用の X.bin / y.bin
// ---------------------------------------------------------------------------
// 入力: PackedSfenValue ×N (40 byte/record)
// 出力: <prefix>.X.bin  float32 [N, dims] row-major、先手視点
//       <prefix>.y.bin  float32 [N]       先手視点 (手番側の値の符号を直したもの)
//       <prefix>.phase.bin float32 [N]
//       <prefix>.meta.txt
// 使い方 (USI):
//   setoption name HceDumpBinpack value input /path/x.binpack output_prefix /tmp/hce dims 252
// sfen の一覧から PackedSfenValue の file を作る。binpack 経路を試すための道具で、
// 教師データを作るためのものではない (score は 1 行に添えられた値か、無ければ 0)。
// 使い方 (USI):
//   setoption name HceMakeBinpack value input /path/sfen.txt output /tmp/x.binpack
void hce_make_binpack_cmd(const std::string& args) {
	DumpArgs a;
	if (!parse_dump_args(args, a, "HceMakeBinpack")) return;
	if (a.input.empty() || a.output.empty()) {
		sync_cout << "info string HceMakeBinpack: input and output are required" << sync_endl;
		return;
	}
	std::ifstream ifs(a.input);
	if (!ifs) {
		sync_cout << "info string HceMakeBinpack: failed to open " << a.input << sync_endl;
		return;
	}
	std::ofstream ofs(a.output, std::ios::binary);
	if (!ofs) {
		sync_cout << "info string HceMakeBinpack: failed to write " << a.output << sync_endl;
		return;
	}

	Position  pos;
	StateInfo si;
	std::string line;
	uint64_t done = 0, bad = 0;
	while (std::getline(ifs, line)) {
		std::string sfen = strip_sfen_line(line);
		if (sfen.empty()) continue;
		// 行末に " score 123" が付いていたら教師値として使う。
		int score = 0;
		const size_t at = sfen.rfind(" score ");
		if (at != std::string::npos) {
			score = std::atoi(sfen.c_str() + at + 7);
			sfen = sfen.substr(0, at);
		}
		if (pos.set(sfen, &si).has_value()) { ++bad; continue; }

		HcePackedSfenValue psv{};
		pos.sfen_pack(psv.sfen);
		psv.score   = (int16_t)std::max(-32000, std::min(32000, score));
		psv.gamePly = (uint16_t)pos.game_ply();
		ofs.write(reinterpret_cast<const char*>(&psv), sizeof(psv));
		++done;
	}
	ofs.flush();
	sync_cout << "info string HceMakeBinpack: wrote " << done << " records"
	          << (bad ? (" (" + std::to_string(bad) + " bad)") : std::string())
	          << " to " << a.output << sync_endl;
}

void hce_dump_binpack_cmd(const std::string& args) {
	DumpArgs a;
	if (!parse_dump_args(args, a, "HceDumpBinpack")) return;
	if (a.input.empty()) {
		sync_cout << "info string HceDumpBinpack: input is required" << sync_endl;
		return;
	}

	const size_t REC = sizeof(HcePackedSfenValue);
	std::ifstream ifs(a.input, std::ios::binary);
	if (!ifs) {
		sync_cout << "info string HceDumpBinpack: failed to open " << a.input << sync_endl;
		return;
	}
	ifs.seekg(0, std::ios::end);
	const uint64_t file_size = (uint64_t)ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	const uint64_t total = file_size / REC;
	if (a.skip >= total) {
		sync_cout << "info string HceDumpBinpack: skip >= records (" << total << ")" << sync_endl;
		return;
	}
	uint64_t n = total - a.skip;
	if (a.max_positions) n = std::min(n, a.max_positions);

	if (a.skip)
		ifs.seekg((std::streamoff)(a.skip * REC), std::ios::beg);

	std::vector<HcePackedSfenValue> psvs(n);
	ifs.read(reinterpret_cast<char*>(psvs.data()), (std::streamsize)(n * REC));
	if ((uint64_t)ifs.gcount() != n * REC) {
		sync_cout << "info string HceDumpBinpack: short read" << sync_endl;
		return;
	}
	ifs.close();

	const int D = a.dims;
	std::vector<float> X((size_t)n * D);
	std::vector<float> y(n), ph(n);
	std::vector<uint8_t> valid(n, 0);

	{
		Position  pos;
		StateInfo si;
		for (uint64_t i = 0; i < n; ++i) {
			const auto& psv = psvs[(size_t)i];
			if (pos.set_from_packed_sfen(psv.sfen, &si).is_not_ok())
				continue;
			extract_dims(pos, a.layout, a.kind, X.data() + (size_t)i * D);
			// psv.score は手番側から見た値。feature は先手視点なので揃える。
			float ys = (float)psv.score;
			if (pos.side_to_move() == WHITE) ys = -ys;
			y[(size_t)i]  = ys;
			ph[(size_t)i] = game_phase(pos);
			valid[(size_t)i] = 1;
		}
	}

	uint64_t kept = 0;
	for (uint64_t i = 0; i < n; ++i) {
		if (!valid[i]) continue;
		if (kept != i) {
			std::memcpy(X.data() + kept * D, X.data() + i * D, sizeof(float) * D);
			y[kept]  = y[i];
			ph[kept] = ph[i];
		}
		++kept;
	}

	const std::string xpath = a.output_prefix + ".X.bin";
	const std::string ypath = a.output_prefix + ".y.bin";
	const std::string ppath = a.output_prefix + ".phase.bin";
	const std::string mpath = a.output_prefix + ".meta.txt";
	{
		std::ofstream ofs(xpath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(X.data()),
		          (std::streamsize)(sizeof(float) * kept * D));
	}
	{
		std::ofstream ofs(ypath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(y.data()), (std::streamsize)(sizeof(float) * kept));
	}
	{
		std::ofstream ofs(ppath, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(ph.data()), (std::streamsize)(sizeof(float) * kept));
	}
	{
		std::ofstream ofs(mpath);
		ofs << "N=" << kept << "\n"
		    << "D=" << D << "\n"
		    << "material_features=" << a.layout.material << "\n"
		    << "mobility_features=" << a.layout.mobility << "\n"
		    << "king_features=" << NUM_KING_FEATURES << "\n"
		    << "contact_features=" << a.layout.contact_dim << "\n"
		    << "variant=" << variant_name(a.layout.variant) << "\n"
		    << "offsets=" << OFF_MATERIAL << "," << a.layout.off_mobility() << ","
		    << a.layout.off_king() << "\n"
		    << "phased=" << (a.kind == 2 ? 1 : 0) << "\n"
		    << "pov=black\n"
		    << "dtype=float32\n"
		    << "X_shape=(N,D) row-major\n";
		if (a.names && a.kind != 0)
			for (int i = 0; i < a.layout.features(); ++i)
				ofs << "col" << i << "=" << feature_v2_name(a.layout, i) << "\n";
	}

	sync_cout << "info string HceDumpBinpack: wrote " << kept << " rows (D=" << D
	          << ") to " << xpath << sync_endl;
}

} // namespace HumanLike
} // namespace Eval
} // namespace YaneuraOu

#endif // HCE_DUMP_INCLUDED_FROM_HUMANLIKE_EVAL || HCE_DUMP_STANDALONE_TU
