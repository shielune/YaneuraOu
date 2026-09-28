#include "../../config.h"

#if defined(USE_HCE_EXPLAIN)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../types.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../../misc.h"
#include "../../usioption.h"
#include "../humanlike/humanlike_eval.h"

// "explain" コマンドの実体。NNUE 系の edition に、HCE v2 の線形評価を
// 説明専用の副評価器として持ち込む。
//
// 探索からは一度も呼ばない。評価値は NNUE のまま変わらず、explain が出す
// black_pov はこの副評価器の値であって、NNUE の評価値ではない。
//
// 重みは rank+contact の 4474 個を hce_explain_weights_embedded.cpp に埋め込んである。
// HceWeightsFile で別の幅のファイルに差し替えられ、版と種類は読んだ個数で決まる
// (HumanLike::layout_for_dim)。読めなかったときは埋め込みに戻る。
//
// Makefile で HCE_EXPLAIN=ON にしたときだけコンパイルされる。

namespace YaneuraOu {
namespace Eval {
namespace Hce {

extern const int   kEmbeddedHceWeightsSize;
extern const float kEmbeddedHceWeights[];

namespace {

struct LinearWeights {
	std::vector<float> w;
	int dim = 0;              // 0 ならまだ何も読んでいない
	HumanLike::Layout layout;
	int kind = 1;             // 0 利きだけ / 1 v2 / 2 進行度で按分
	double bias = 0.0;        // ファイルの末尾に切片があればその値
	const char* source = "none";
};

LinearWeights g_w;

inline double clamp3000(double v) {
	return v > 3000.0 ? 3000.0 : (v < -3000.0 ? -3000.0 : v);
}

// 重みの並びを w に入れる。n 個が既知の幅なら切片なし、n-1 個が既知の幅なら
// 末尾の 1 個を切片として扱う。どちらでもなければ false。
bool assign_weights(const float* v, int n, const char* source) {
	HumanLike::Layout layout;
	int kind = 0, dim = 0;
	bool has_bias = false;
	if (HumanLike::layout_for_dim(n, layout, kind)) dim = n;
	else if (n >= 1 && HumanLike::layout_for_dim(n - 1, layout, kind)) { dim = n - 1; has_bias = true; }
	else return false;

	g_w.w.assign(v, v + dim);
	g_w.dim    = dim;
	g_w.layout = layout;
	g_w.kind   = kind;
	g_w.bias   = has_bias ? v[dim] : 0.0;
	g_w.source = source;
	return true;
}

void use_embedded() {
	if (!assign_weights(kEmbeddedHceWeights, kEmbeddedHceWeightsSize, "embedded"))
		g_w = LinearWeights();
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

// 空なら埋め込みに戻す。読めないとき、幅が合わないときも埋め込みに戻す。
void load_linear_weights(const std::string& path) {
	if (path.empty()) { g_w = LinearWeights(); return; }
	std::vector<float> v;
	if (!read_float_list(path, v)) {
		sync_cout << "info string HceWeightsFile: failed to read " << path << sync_endl;
		use_embedded();
		sync_cout << "info string HceWeightsFile: using embedded weights" << sync_endl;
		return;
	}
	const int n = (int)v.size();
	if (!assign_weights(v.data(), n, "file")) {
		sync_cout << "info string HceWeightsFile: rejected: " << n << " values is not a known HCE width" << sync_endl;
		use_embedded();
		sync_cout << "info string HceWeightsFile: using embedded weights" << sync_endl;
		return;
	}
	sync_cout << "info string HceWeightsFile: loaded " << g_w.dim << " weights (variant " << HumanLike::variant_name(g_w.layout.variant) << ", pieces " << HumanLike::pieces_name(g_w.layout.pieces) << ")" << sync_endl;
}

// この局面での、layout.features() 列それぞれの実効的な重み。
// 按分した重みは前半が x*(1-t)、後半が x*t なので、進行度で畳んで 1 本にする。
void effective_weights(const Position& pos, const HumanLike::Layout& layout, double* w) {
	const int n = layout.features();
	std::fill(w, w + n, 0.0);
	if (g_w.kind == 0) {
		for (int i = 0; i < g_w.dim; ++i)
			w[layout.off_mobility() + i] = (double)g_w.w[i];
		return;
	}
	if (g_w.kind == 1) {
		for (int i = 0; i < n; ++i)
			w[i] = (double)g_w.w[i];
		return;
	}
	const double t = (double)HumanLike::game_phase(pos);
	for (int i = 0; i < n; ++i)
		w[i] = (double)g_w.w[i] * (1.0 - t) + (double)g_w.w[n + i] * t;
}

// JSON の文字列に出せない字を落とす。列の名前は自前で組んでいるので " と \
// くらいしか来ないが、SFEN と指し手も同じ道を通すのでまとめて面倒を見る。
std::string json_quote(const std::string& v) {
	std::string out = "\"";
	for (char c : v) {
		if (c == '"' || c == '\\') { out += '\\'; out += c; }
		else if ((unsigned char)c < 0x20)  out += ' ';
		else out += c;
	}
	out += '"';
	return out;
}

// 小数は要らない。評価値の単位は内部値で、1 の位より下は読む意味がない。
std::string num(double v) {
	std::ostringstream ss;
	ss << (long long)std::llround(v);
	return ss.str();
}

// see_ge は「しきい値以上か」しか答えないので、二分探索で数値に直す。
// 探す幅は駒がぶつかり合って動きうる範囲 (飛車二枚ぶんで足りる) にとってある。
// 16 回でこの幅を 1 まで詰められる。
int see_value(const Position& pos, Move m) {
	int lo = -4000, hi = 4000;   // lo は必ず満たす側、hi は満たさないかもしれない側
	if (!pos.see_ge(m, (Value)lo)) return lo;
	if (pos.see_ge(m, (Value)hi))  return hi;
	while (hi - lo > 1) {
		const int mid = lo + (hi - lo) / 2;
		if (pos.see_ge(m, (Value)mid)) lo = mid; else hi = mid;
	}
	return lo;
}

// 一つの節点の、列ごとの寄与。先手視点で、切片の前。
struct Snapshot {
	std::vector<double> contrib;   // layout.features() 個
	double mat = 0.0, mob = 0.0, kng = 0.0;
};

void take_snapshot(const Position& pos, const HumanLike::Layout& layout, Snapshot& out) {
	const int n = layout.features();
	std::vector<float> feat((size_t)n);
	HumanLike::extract_features_v2(pos, layout, feat.data());
	std::vector<double> w((size_t)n);
	effective_weights(pos, layout, w.data());

	out.contrib.assign((size_t)n, 0.0);
	out.mat = out.mob = out.kng = 0.0;
	for (int i = 0; i < n; ++i) {
		const double c = w[i] * (double)feat[i];
		out.contrib[(size_t)i] = c;
		if      (i < layout.off_mobility()) out.mat += c;
		else if (i < layout.off_king())       out.mob += c;
		else                                  out.kng += c;
	}
}

// 直前の節点から寄与が動いた列を、動いた量の大きい順に topn 個。
std::string top_movers(const HumanLike::Layout& layout,
                       const Snapshot& before, const Snapshot& after, int topn) {
	const int n = layout.features();
	std::vector<int> order;
	order.reserve((size_t)n);
	for (int i = 0; i < n; ++i)
		if (before.contrib[(size_t)i] != after.contrib[(size_t)i])
			order.push_back(i);
	std::sort(order.begin(), order.end(), [&](int a, int b) {
		return std::abs(after.contrib[(size_t)a] - before.contrib[(size_t)a])
		     > std::abs(after.contrib[(size_t)b] - before.contrib[(size_t)b]);
	});
	if ((int)order.size() > topn)
		order.resize((size_t)topn);

	std::string out = "[";
	for (size_t k = 0; k < order.size(); ++k) {
		const int i = order[k];
		// feature_v2_name は static な文字列を使い回すので、その場で写す。
		const std::string name = HumanLike::feature_v2_name(layout, i);
		if (k) out += ",";
		out += "{\"name\":" + json_quote(name)
		     + ",\"d\":" + num(after.contrib[(size_t)i] - before.contrib[(size_t)i]) + "}";
	}
	out += "]";
	return out;
}

// 一つの節点ぶんの JSON。move が MOVE_NONE なら根の局面。
void emit_node(const Position& pos, const HumanLike::Layout& layout,
               int ply, Move move, int see, bool gives_check,
               const Snapshot* before, const Snapshot& now, int topn) {
	std::string line = "{\"ply\":" + std::to_string(ply);
	if (move != Move::none()) {
		line += ",\"move\":" + json_quote(to_usi_string(move))
		      + ",\"see\":" + std::to_string(see)
		      + ",\"gives_check\":" + (gives_check ? "true" : "false");
	}
	line += ",\"sfen\":" + json_quote(pos.sfen())
	      + ",\"black_pov\":" + num(clamp3000(now.mat + now.mob + now.kng + g_w.bias))
	      + ",\"material\":" + num(now.mat)
	      + ",\"mobility\":" + num(now.mob)
	      + ",\"king\":" + num(now.kng);
	if (before) {
		line += ",\"d_material\":" + num(now.mat - before->mat)
		      + ",\"d_mobility\":" + num(now.mob - before->mob)
		      + ",\"d_king\":" + num(now.kng - before->kng)
		      + ",\"top\":" + top_movers(layout, *before, now, topn);
	}
	line += "}";
	sync_cout << line << sync_endl;
}

} // namespace
} // namespace Hce

void add_hce_explain_options(OptionsMap& options) {
	// 空のままなら、最初の explain で埋め込みの重みを使う。
	options.add("HceWeightsFile", Option("", [](const Option& o) {
		Hce::load_linear_weights((std::string)o);
		return std::nullopt;
	}));
}

void hce_explain(Position& pos, const std::vector<Move>& pv, int topn) {
	using namespace Hce;

	if (g_w.dim == 0)
		use_embedded();
	if (g_w.dim == 0) {
		sync_cout << "{\"error\":" << json_quote("no weights loaded") << "}" << sync_endl;
		return;
	}
	const HumanLike::Layout layout = g_w.layout;

	Snapshot root;
	take_snapshot(pos, layout, root);

	// 六つの版は合計こそ揃うが、駒得・利き・玉への配り方が版ごとに大きく違う。
	// 項の絶対値を単独で読むと版を跨いだ途端に話が合わなくなるので、読んでよい
	// のは同じ版の中での差分だけ、という断りを出力自体に入れておく。
	// weights はどちらの重みで計算したか。ファイルが読めないと黙って埋め込みに
	// 戻るので、出力を見ただけでわかるようにしておく。
	sync_cout << "{\"route\":\"linear\""
	          << ",\"weights\":" << json_quote(g_w.source)
	          << ",\"variant\":" << json_quote(HumanLike::variant_name(layout.variant))
	          << ",\"pieces\":" << json_quote(HumanLike::pieces_name(layout.pieces))
	          << ",\"features\":" << layout.features()
	          << ",\"phase\":" << HumanLike::game_phase(pos)
	          << ",\"pov\":\"black\""
	          << ",\"caveat\":" << json_quote(
	                 "項の絶対値は版ごとに配り方が違う。読んでよいのは同じ版の中での差分だけ。")
	          << "}" << sync_endl;

	emit_node(pos, layout, 0, Move::none(), 0, false, nullptr, root, topn);

	// 指した手は最後に全部戻すので、StateInfo は寿命が要る。deque なら
	// 足しても前の要素が動かない。
	std::deque<StateInfo> states;
	std::vector<Move> played;
	Snapshot before = root;

	for (size_t k = 0; k < pv.size(); ++k) {
		const Move m = pv[k];
		// 第二引数は「不成も含めて生成した手か」。explain は人が並べた読み筋を
		// 受けるので、不成の手も弾かずに通す。
		if (m == Move::none() || !pos.pseudo_legal(m, true) || !pos.legal(m)) {
			sync_cout << "{\"ply\":" << (k + 1)
			          << ",\"move\":" << json_quote(to_usi_string(m))
			          << ",\"error\":\"illegal move\"}" << sync_endl;
			break;
		}
		// 指す前の局面でないと意味がない二つ。
		const int  see   = see_value(pos, m);
		const bool check = pos.gives_check(m);

		states.emplace_back();
		pos.do_move(m, states.back());
		played.push_back(m);

		Snapshot now;
		take_snapshot(pos, layout, now);
		emit_node(pos, layout, (int)k + 1, m, see, check, &before, now, topn);
		before = now;
	}

	for (size_t k = played.size(); k-- > 0; )
		pos.undo_move(played[k]);
}

} // namespace Eval
} // namespace YaneuraOu

#endif // USE_HCE_EXPLAIN
