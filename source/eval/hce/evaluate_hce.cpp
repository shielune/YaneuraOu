#include "../../config.h"

#if defined(EVAL_MOBILITY)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../../types.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../../misc.h"
#include "../../usioption.h"
#include "../humanlike/humanlike_eval.h"
#include "hce_mlp.h"

#if defined(__has_include)
#  if __has_include("hce_mlp_weights_embedded.h")
#    include "hce_mlp_weights_embedded.h"
#  endif
#endif

// HCE (手作り評価関数) を embedded weights で動かす standalone eval。
// NNUE と違って外部 nn.bin を要求しない。
//
// 評価の経路は三つ。USI option の HceRoute で選ぶ。
//
//   legacy164 (既定) … 従来どおり。164 次元 × 焼き込み重みの内積ひとつ。
//                       option を何も触らなければ変更前と 1 の位まで同じ値を返す。
//   linear          … HceWeightsFile で読んだ重みとの内積。版と種類は読んだ個数で決まる。
//                       六つの版それぞれに「利きだけ」「駒得＋利き＋玉」「進行度で按分」
//                       「駒得＋利き＋玉＋手番の 2 列」の四つの幅があり、24 通りはすべて
//                       異なるので取り違えない。切片あり。
//   mlp             … plain の 231 次元を入力とする全結合層 (231 → 32 → 32 → 1、ReLU)。
//
// 切片の置き場所は二つに分けてある。
//   HceBias  … 先手番から見た定数。符号反転の *前* に足す。Ridge の intercept はこちら。
//   HceTempo … 手番側から見た定数。符号反転の *後* に足す。手番の得はこちら。
//
// 重みの焼き込みは tideborn/tools/embed_hce_weights.py が
// mobility_weights_embedded.cpp に書く。

namespace YaneuraOu {
namespace Eval { namespace Mobility {

extern const float kEmbeddedMobilityWeights[164];
extern const int   kEmbeddedMobilityWeightsSize;

// ---------------------------------------------------------------------------
// JSON から全結合層の重みを読む
// ---------------------------------------------------------------------------
namespace {

// s の pos 以降から key に対応する値の並びを取り出す。
// "key" : [ ... ] でも "key" : 数値 でもよく、[] の入れ子は無視して中の数値を順に拾う。
bool json_numbers_after_key(const std::string& s, const std::string& key,
                            std::vector<float>& out) {
	const std::string quoted = "\"" + key + "\"";
	size_t k = s.find(quoted);
	if (k == std::string::npos) return false;
	size_t c = s.find(':', k + quoted.size());
	if (c == std::string::npos) return false;

	size_t i = c + 1;
	while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
	if (i >= s.size()) return false;

	out.clear();
	if (s[i] == '[') {
		int depth = 0;
		for (; i < s.size(); ++i) {
			const char ch = s[i];
			if (ch == '[') { ++depth; continue; }
			if (ch == ']') { if (--depth == 0) return true; continue; }
			if (ch == '-' || ch == '+' || ch == '.' || std::isdigit((unsigned char)ch)) {
				const char* begin = s.c_str() + i;
				char* end = nullptr;
				double v = std::strtod(begin, &end);
				if (end == begin) return false;
				out.push_back((float)v);
				i += (size_t)(end - begin) - 1;
			}
		}
		return false; // ] が閉じなかった
	}

	const char* begin = s.c_str() + i;
	char* end = nullptr;
	double v = std::strtod(begin, &end);
	if (end == begin) return false;
	out.push_back((float)v);
	return true;
}

bool json_grab(const std::string& s, const std::vector<std::string>& keys,
               std::vector<float>& out) {
	for (const auto& k : keys)
		if (json_numbers_after_key(s, k, out)) return true;
	return false;
}

bool grab_exact(const std::string& s, const std::vector<std::string>& keys,
                size_t want, float* dst, const char* what, std::string& err) {
	std::vector<float> v;
	if (!json_grab(s, keys, v)) {
		err = std::string(what) + ": key not found (tried " + keys[0] + " etc.)";
		return false;
	}
	if (v.size() != want) {
		err = std::string(what) + ": expected " + std::to_string(want)
		    + " numbers, got " + std::to_string(v.size());
		return false;
	}
	std::copy(v.begin(), v.end(), dst);
	return true;
}

} // namespace

bool load_hce_mlp_json(const std::string& path, HceMlp& out, std::string& err) {
	std::ifstream ifs(path);
	if (!ifs) { err = "failed to open " + path; return false; }
	std::stringstream ss;
	ss << ifs.rdbuf();
	const std::string s = ss.str();

	if (!grab_exact(s, {"w1", "fc1.weight", "net.0.weight", "layers.0.weight", "0.weight"},
	                (size_t)HCE_MLP_H1 * HCE_MLP_IN, out.w1, "w1", err)) return false;
	if (!grab_exact(s, {"b1", "fc1.bias", "net.0.bias", "layers.0.bias", "0.bias"},
	                HCE_MLP_H1, out.b1, "b1", err)) return false;
	if (!grab_exact(s, {"w2", "fc2.weight", "net.2.weight", "layers.2.weight", "2.weight"},
	                (size_t)HCE_MLP_H2 * HCE_MLP_H1, out.w2, "w2", err)) return false;
	if (!grab_exact(s, {"b2", "fc2.bias", "net.2.bias", "layers.2.bias", "2.bias"},
	                HCE_MLP_H2, out.b2, "b2", err)) return false;
	if (!grab_exact(s, {"w3", "fc3.weight", "net.4.weight", "layers.4.weight", "4.weight"},
	                HCE_MLP_H2, out.w3, "w3", err)) return false;
	{
		float b3[1];
		if (!grab_exact(s, {"b3", "fc3.bias", "net.4.bias", "layers.4.bias", "4.bias"},
		                1, b3, "b3", err)) return false;
		out.b3 = b3[0];
	}
	return true;
}

} } // namespace Eval::Mobility

namespace Eval {

namespace {

using Mobility::HceMlp;

enum class Route { Legacy164, Linear, Mlp };

Route  g_route = Route::Legacy164;

// linear 経路の重み。個数で次元が決まる (164 / 231 / 462)。
// 末尾に 1 個余分にあれば、それは切片として扱う。
std::vector<float> g_linear_w;
int    g_linear_dim  = 0;
// 読んだ重みの個数から決まる並びと種類 (0 利きだけ / 1 v2 / 2 按分)。
HumanLike::Layout g_linear_layout;
int    g_linear_kind = 1;
double g_file_bias   = 0.0;   // 重みファイルの末尾から来た切片
double g_opt_bias    = 0.0;   // HceBias
double g_opt_tempo   = 0.0;   // HceTempo

bool g_mlp_is_placeholder = true;

// Eval::init() はこの edition では呼ばれない (yaneuraou_ffi.cpp からしか呼ばれていない)。
// 初期化を init() に置くと、全結合層の重みが 0 のまま使われて経路が死ぬ。
// 最初に触ったときに一度だけ用意する。
HceMlp& mlp() {
	static HceMlp m = [] {
		HceMlp t;
		t.fill_placeholder();
#if defined(HCE_MLP_EMBEDDED)
		std::copy(std::begin(Mobility::kHceMlpW1), std::end(Mobility::kHceMlpW1), t.w1);
		std::copy(std::begin(Mobility::kHceMlpB1), std::end(Mobility::kHceMlpB1), t.b1);
		std::copy(std::begin(Mobility::kHceMlpW2), std::end(Mobility::kHceMlpW2), t.w2);
		std::copy(std::begin(Mobility::kHceMlpB2), std::end(Mobility::kHceMlpB2), t.b2);
		std::copy(std::begin(Mobility::kHceMlpW3), std::end(Mobility::kHceMlpW3), t.w3);
		t.b3 = Mobility::kHceMlpB3;
		g_mlp_is_placeholder = false;
#endif
		return t;
	}();
	return m;
}

// 焼き込み重みと全結合層はどちらも plain の並びを使う。
const HumanLike::Layout& plain_layout() {
	static const HumanLike::Layout L = HumanLike::make_layout(HumanLike::Variant::Plain);
	return L;
}

inline double clamp3000(double v) {
	return v > 3000.0 ? 3000.0 : (v < -3000.0 ? -3000.0 : v);
}

double parse_double(const std::string& s, double fallback) {
	const char* begin = s.c_str();
	char* end = nullptr;
	double v = std::strtod(begin, &end);
	return (end == begin) ? fallback : v;
}

bool read_float_list(const std::string& path, std::vector<float>& out) {
	std::ifstream ifs(path); if (!ifs) return false;
	out.clear(); std::string line;
	while (std::getline(ifs,line)) {
		size_t p=line.find_first_not_of(" \t\r\n"); if(p==std::string::npos || line[p]=='#') continue;
		const char* begin=line.c_str()+p;
		while(*begin){ char* end=nullptr; double v=std::strtod(begin,&end); if(end==begin)return false; out.push_back((float)v); begin=end; while(*begin==','||*begin==' '||*begin=='\t'||*begin=='\r')++begin; }
	}
	return true;
}

void load_linear_weights(const std::string& path) {
	if(path.empty()) return;
	std::vector<float> v;
	if(!read_float_list(path,v)){ g_linear_dim=0; sync_cout << "info string HceWeightsFile: failed to read " << path << sync_endl; return; }
	HumanLike::Layout layout; int kind=0, dim=0; bool has_bias=false; const int n=(int)v.size();
	if(HumanLike::layout_for_dim(n,layout,kind)) dim=n;
	else if(n>=1 && HumanLike::layout_for_dim(n-1,layout,kind)){dim=n-1;has_bias=true;}
	else {g_linear_dim=0;sync_cout << "info string HceWeightsFile: rejected: " << n << " values is not a known HCE width" << sync_endl;return;}
	g_linear_w.assign(v.begin(),v.begin()+dim);g_linear_dim=dim;g_linear_layout=layout;g_linear_kind=kind;g_file_bias=has_bias?v[dim]:0.0;
	sync_cout << "info string HceWeightsFile: loaded " << dim << " weights (variant " << HumanLike::variant_name(layout.variant) << ", pieces " << HumanLike::pieces_name(layout.pieces) << ")" << sync_endl;
}

void load_mlp_weights(const std::string& path) {
	if (path.empty()) return;
	std::string err;
	HceMlp tmp;
	if (!Mobility::load_hce_mlp_json(path, tmp, err)) {
		sync_cout << "info string HceMlpFile: " << err << sync_endl;
		return;
	}
	mlp() = tmp;
	g_mlp_is_placeholder = false;
	sync_cout << "info string HceMlpFile: loaded " << path << sync_endl;
}

// 先手番から見た素の評価値 (頭打ち・符号反転の前)。
double raw_black_pov(const Position& pos) {
	switch (g_route) {
	case Route::Legacy164: {
		float feat[HumanLike::NUM_BOARD_FEATURES_FOLDED + HumanLike::NUM_HAND_FEATURES];
		HumanLike::extract_features(pos, plain_layout(), feat);
		// 焼き込んである重みが 164 個に満たなくても、ある分までしか足さない。
		const int n = std::min(Mobility::kEmbeddedMobilityWeightsSize,
		                       HumanLike::NUM_BOARD_FEATURES_FOLDED + HumanLike::NUM_HAND_FEATURES);
		double sum = 0.0;
		for (int i = 0; i < n; ++i)
			sum += (double)Mobility::kEmbeddedMobilityWeights[i] * (double)feat[i];
		return sum;
	}
	case Route::Linear: {
		if (g_linear_dim == 0) return 0.0;
		// 特徴を並べずに、利きを数えながら内積を足していく。
		// square+contact は 12111 列あるので、毎回ゼロ埋めしていたら探索が持たない。
		return HumanLike::linear_value(pos, g_linear_layout, g_linear_kind, g_linear_w.data());
	}
	case Route::Mlp: {
		float feat[Mobility::HCE_MLP_IN];
		HumanLike::extract_features_v2(pos, plain_layout(), feat);
		return (double)mlp().forward(feat);
	}
	}
	return 0.0;
}

// この局面での、layout.features() 列それぞれの実効的な重み。
// legacy164 は利きの 164 列だけに重みがあり、駒得と玉の安全度は 0。
// 按分した重みは前半が x*(1-t)、後半が x*t なので、進行度で畳んで 1 本にする。
// 手番の 2 列 (kind 3) はここには入れない。print_eval_stat が別に出す。
// 重みが無いときと mlp のときは false を返す。全結合層の出力は
// ブロックごとの和ではないので、寄与を分けて出すこと自体ができない。
bool effective_weights(const Position& pos, const HumanLike::Layout& layout, double* w) {
	const int n = layout.features();
	std::fill(w, w + n, 0.0);
	switch (g_route) {
	case Route::Legacy164: {
		const int m = std::min(Mobility::kEmbeddedMobilityWeightsSize,
		                       HumanLike::NUM_BOARD_FEATURES_FOLDED + HumanLike::NUM_HAND_FEATURES);
		for (int i = 0; i < m; ++i)
			w[layout.off_mobility() + i] = (double)Mobility::kEmbeddedMobilityWeights[i];
		return true;
	}

	case Route::Linear:
		if (g_linear_dim == 0) return false;
		if (g_linear_kind == 0) {
			for (int i = 0; i < g_linear_dim; ++i)
				w[layout.off_mobility() + i] = (double)g_linear_w[i];
			return true;
		}
		if (g_linear_kind == 1 || g_linear_kind == 3) {
			for (int i = 0; i < n; ++i)
				w[i] = (double)g_linear_w[i];
			return true;
		}
		{
			const double t = (double)HumanLike::game_phase(pos);
			for (int i = 0; i < n; ++i)
				w[i] = (double)g_linear_w[i] * (1.0 - t) + (double)g_linear_w[n + i] * t;
		}
		return true;

	case Route::Mlp:
		return false;
	}
	return false;
}

const std::vector<std::string> kRouteNames = { "legacy164", "linear", "mlp" };

} // namespace

void init() { (void)mlp(); }

void add_options(OptionsMap& options, ThreadPool&) {
	// 既定は従来の経路。何も触らなければ変更前と同じ動きをする。
	(void)mlp(); // 全結合層の重みをここで用意しておく

	options.add("HceRoute", Option(kRouteNames, kRouteNames[0], [](const Option& o) {
		const std::string v = (std::string)o;
		g_route = (v == "linear") ? Route::Linear : (v == "mlp") ? Route::Mlp : Route::Legacy164;
		if (g_route == Route::Linear && g_linear_dim == 0)
			sync_cout << "info string HceRoute=linear but no weights loaded;"
			             " set HceWeightsFile first (eval is bias only)" << sync_endl;
		if (g_route == Route::Mlp && g_mlp_is_placeholder)
			sync_cout << "info string HceRoute=mlp is running on PLACEHOLDER weights;"
			             " set HceMlpFile for real ones" << sync_endl;
		return std::nullopt;
	}));

	options.add("HceWeightsFile", Option("", [](const Option& o) {
		load_linear_weights((std::string)o);
		return std::nullopt;
	}));

	options.add("HceMlpFile", Option("", [](const Option& o) {
		load_mlp_weights((std::string)o);
		return std::nullopt;
	}));

	// 切片。内部単位 (センチポーン × 0.9)。小数で書ける。
	options.add("HceBias", Option("0", [](const Option& o) {
		g_opt_bias = parse_double((std::string)o, 0.0);
		return std::nullopt;
	}));
	options.add("HceTempo", Option("0", [](const Option& o) {
		g_opt_tempo = parse_double((std::string)o, 0.0);
		return std::nullopt;
	}));

	// Rust 側の dump と突き合わせるための出力。
	// 値は sfen そのものだが、頭に "variant <名前> " と書けば版を選べる。
	options.add("HceDumpSfen", Option("", [](const Option& o) {
		std::string v = (std::string)o;
		if (v.empty()) return std::nullopt;
		HumanLike::Layout layout = plain_layout();
		if (v.rfind("variant ", 0) == 0) {
			const size_t sp = v.find(' ', 8);
			const std::string name = v.substr(8, sp == std::string::npos ? sp : sp - 8);
			HumanLike::Variant parsed;
			if (!HumanLike::parse_variant(name, parsed)) {
				sync_cout << "info string HceDumpSfen: unknown variant: " << name << sync_endl;
				return std::nullopt;
			}
			layout = HumanLike::make_layout(parsed);
			v = (sp == std::string::npos) ? std::string() : v.substr(sp + 1);
		}
		HumanLike::hce_dump_sfen(std::cout, layout, v);
		std::cout.flush();
		return std::nullopt;
	}));
	options.add("HceDumpFile", Option("", [](const Option& o) {
		const std::string v = (std::string)o;
		if (!v.empty()) HumanLike::hce_dump_file_cmd(v);
		return std::nullopt;
	}));
	options.add("HceDumpBinpack", Option("", [](const Option& o) {
		const std::string v = (std::string)o;
		if (!v.empty()) HumanLike::hce_dump_binpack_cmd(v);
		return std::nullopt;
	}));

	// sfen の一覧から PackedSfenValue の file を作る (上の binpack 経路を試すため)。
	options.add("HceMakeBinpack", Option("", [](const Option& o) {
		const std::string v = (std::string)o;
		if (!v.empty()) HumanLike::hce_make_binpack_cmd(v);
		return std::nullopt;
	}));
}

void load_eval() {
	std::cerr << "info string EVAL_MOBILITY: using embedded mobility weights ("
	          << Mobility::kEmbeddedMobilityWeightsSize << " params)" << std::endl;
}

void print_eval_stat(Position& pos) {
	// linear のときは読んだ重みが決めた版、それ以外は plain。
	const HumanLike::Layout layout =
	    (g_route == Route::Linear && g_linear_dim != 0) ? g_linear_layout : plain_layout();
	const int n = layout.features();

	std::vector<float> feat((size_t)n);
	HumanLike::extract_features_v2(pos, layout, feat.data());
	const float t = HumanLike::game_phase(pos);

	std::vector<double> w((size_t)n);
	const bool separable = effective_weights(pos, layout, w.data());
	// 手番の 2 列は読んだ重みにあるときだけ。HceTempo の値とは別物。
	const bool turn_columns = g_route == Route::Linear && g_linear_dim != 0 && g_linear_kind == 3;

	std::cout << "--- EVAL STAT: EVAL_MOBILITY" << std::endl
	          << "  route    = " << kRouteNames[(int)g_route] << std::endl
	          << "  variant  = " << HumanLike::variant_name(layout.variant)
	          << " (" << (turn_columns ? layout.with_tempo() : n) << " 列)" << std::endl
	          << "  phase    = " << t << std::endl;

	if (separable) {
		// 内積なので、三つの和は切片と符号反転の前の評価値そのものになる。
		double mat = 0.0, mob = 0.0, kng = 0.0;
		for (int i = 0; i < n; ++i) {
			const double c = w[i] * (double)feat[i];
			if      (i < layout.off_mobility()) mat += c;
			else if (i < layout.off_king())       mob += c;
			else                                  kng += c;
		}
		std::cout << "  material = " << mat << std::endl
		          << "  mobility = " << mob << std::endl
		          << "  king     = " << kng << std::endl;
		double turn = 0.0;
		if (turn_columns) {
			float z[HumanLike::NUM_TEMPO_FEATURES];
			HumanLike::extract_tempo_features(pos, z);
			for (int k = 0; k < HumanLike::NUM_TEMPO_FEATURES; ++k)
				turn += (double)g_linear_w[(size_t)n + k] * (double)z[k];
			std::cout << "  turn     = " << turn << " (重みの手番の 2 列)" << std::endl;
		}
		std::cout << "  subtotal = " << (mat + mob + kng + turn)
		          << " (先手視点、切片の前)" << std::endl;
	} else if (g_route == Route::Mlp) {
		std::cout << "  material = n/a" << std::endl
		          << "  mobility = n/a" << std::endl
		          << "  king     = n/a" << std::endl
		          << "  subtotal = " << raw_black_pov(pos)
		          << " (全結合層の出力はブロックごとの和に分けられない)" << std::endl;
	} else {
		std::cout << "  material = n/a" << std::endl
		          << "  mobility = n/a" << std::endl
		          << "  king     = n/a" << std::endl
		          << "  subtotal = n/a (重みが読めていない。HceWeightsFile を先に指定する)"
		          << std::endl;
	}

	std::cout << "  bias     = " << (g_file_bias + g_opt_bias)
	          << " (file " << g_file_bias << " + option " << g_opt_bias << ")" << std::endl
	          << "  tempo    = " << g_opt_tempo << std::endl
	          << "  eval     = " << (int)Eval::evaluate(pos) << std::endl;
}

void evaluate_with_no_return(const Position& /*pos*/) {
	// 差分計算は持たない。何もしない。
}

Value compute_eval(const Position& pos) {
	double sum = raw_black_pov(pos);

	// 切片は先手番から見た値なので、符号反転の前に足す。
	// 既定は 0 なので、option を触らなければ従来の経路の値は変わらない。
	sum += g_file_bias + g_opt_bias;

	sum = clamp3000(sum);
	int score = (int)sum;
	score = (pos.side_to_move() == BLACK) ? score : -score;

	// 手番の得は手番側から見た値なので、符号反転の後に足す。
	if (g_opt_tempo != 0.0)
		score = (int)clamp3000((double)score + g_opt_tempo);

	return Value(score);
}

Value evaluate(const Position& pos) {
	return compute_eval(pos);
}

} // namespace Eval
} // namespace YaneuraOu

#endif // EVAL_MOBILITY
