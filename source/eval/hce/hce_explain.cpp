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
#include "hce_board.h"
#include "hce_factors.h"
#include "hce_net.h"
#include "hce_material.h"
#include "hce_model.h"
#include "hce_progress.h"
#include "hce_speed.h"

// "explain" コマンドの実体。NNUE 系の edition に、HCE v2 の線形評価を
// 説明専用の副評価器として持ち込む。
//
// 探索からは一度も呼ばない。評価値は NNUE のまま変わらず、explain が出す
// black_pov はこの副評価器の値であって、NNUE の評価値ではない。
//
// 重みは rank+contact の 4474 個に手番の 2 列を足した 4476 個を
// hce_explain_weights_embedded.cpp に埋め込んである。
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
	int kind = 1;             // 0 利きだけ / 1 v2 / 2 進行度で按分 / 3 v2 と手番の 2 列
	double bias = 0.0;        // ファイルの末尾に切片があればその値
	const char* source = "none";
};

LinearWeights g_w;

// 進行度 (終局までの残り手数の推定)。HceProgressFile で読む。読めていなければ出力に出さない。
Progress g_progress;

// 局面を要素に分けるモデル。HceExplainFile で読む。
Model g_model;

// モデルの倍率は、ある進行度のネットワークに合わせて学習してある。違うものと組むと
// 倍率が合わないので、同じものを読んでいるときだけ要素を出す。
// モデルのうち、ネットワークの節を組み立てたもの。
struct Nets {
	Mlp speed;
	Mlp king;
	Mlp activity;
	// 各要素の値のうち、17 列の駒の枚数の差で説明できる部分。要素から引いて駒得に足す。
	// 駒得が「どの要素から見ても駒の価値」になり、ほかの要素には駒の数では言えない分だけが残る。
	float moved_speed[NUM_MATERIAL_COLUMNS] = {};
	float moved_king[NUM_MATERIAL_COLUMNS] = {};
	float moved_activity[NUM_MATERIAL_COLUMNS] = {};
	int king_radius = 3;
	bool built = false;
};
Nets g_nets;

// 節からネットワークを作る。足りない節があれば false。
bool build_nets(const Model& model, std::string& error) {
	Nets nets;
	const Model::Section* sp = model.find("speed");
	if (sp) {
		if (!nets.speed.assign(sp->v, int(sp->p[0]), int(sp->p[1]))) {
			error = "section speed does not match its widths";
			return false;
		}
		if (nets.speed.in() != 8) {
			error = "section speed does not take the eight speed columns";
			return false;
		}
	}
	const Model::Section* kg = model.find("king");
	if (kg) {
		const int radius = int(kg->p[2]);
		if (radius < 1 || radius > 4 || int(kg->p[0]) != window_width(radius)
		    || !nets.king.assign(kg->v, int(kg->p[0]), int(kg->p[1]))) {
			error = "section king does not match its window";
			return false;
		}
		nets.king_radius = radius;
	}
	const Model::Section* ac = model.find("activity");
	if (ac) {
		if (int(ac->p[0]) != PIECE_FACTS || !nets.activity.assign(ac->v, int(ac->p[0]), int(ac->p[1]))) {
			error = "section activity does not match the per-piece facts";
			return false;
		}
	}
	auto moved = [&](const char* name, float* out) {
		const Model::Section* sec = model.find(name);
		if (!sec) return true;   // 無ければ移さない
		if (sec->v.size() != NUM_MATERIAL_COLUMNS) {
			error = std::string("section ") + name + " is not 17 values";
			return false;
		}
		for (int i = 0; i < NUM_MATERIAL_COLUMNS; ++i) out[i] = sec->v[i];
		return true;
	};
	if (!moved("mv_s", nets.moved_speed) || !moved("mv_k", nets.moved_king) || !moved("mv_a", nets.moved_activity))
		return false;
	nets.built = true;
	g_nets = nets;
	return true;
}

bool model_ready() {
	return g_model.loaded() && g_progress.loaded()
	    && g_model.progress_fingerprint() == g_progress.fingerprint();
}

void warn_if_unpaired() {
	if (g_model.loaded() && g_progress.loaded() && !model_ready())
		sync_cout << "info string HceExplainFile: its curves were fitted against another progress"
		             " network than the one in HceProgressFile; the parts are not printed" << sync_endl;
}

// explain に速度の列を付けるか (HceSpeed)。局面ごとに詰みを探すので既定は偽。
bool g_speed = false;

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
// 手番の 2 列はここには入れず、take_snapshot が別に足す。
void effective_weights(const Position& pos, const HumanLike::Layout& layout, double* w) {
	const int n = layout.features();
	std::fill(w, w + n, 0.0);
	if (g_w.kind == 0) {
		for (int i = 0; i < g_w.dim; ++i)
			w[layout.off_mobility() + i] = (double)g_w.w[i];
		return;
	}
	if (g_w.kind == 1 || g_w.kind == 3) {
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
// 手番の列がある重みでは、contrib の末尾に手番の 2 列が続く。
struct Snapshot {
	std::vector<double> contrib;   // layout.features() 個、手番の列があれば +2
	double mat = 0.0, mob = 0.0, kng = 0.0, tmp = 0.0;
};

inline bool has_tempo() { return g_w.kind == 3; }

void take_snapshot(const Position& pos, const HumanLike::Layout& layout, Snapshot& out) {
	const int n = layout.features();
	std::vector<float> feat((size_t)n);
	HumanLike::extract_features_v2(pos, layout, feat.data());
	std::vector<double> w((size_t)n);
	effective_weights(pos, layout, w.data());

	out.contrib.assign((size_t)n + (has_tempo() ? HumanLike::NUM_TEMPO_FEATURES : 0), 0.0);
	out.mat = out.mob = out.kng = out.tmp = 0.0;
	for (int i = 0; i < n; ++i) {
		const double c = w[i] * (double)feat[i];
		out.contrib[(size_t)i] = c;
		if      (i < layout.off_mobility()) out.mat += c;
		else if (i < layout.off_king())       out.mob += c;
		else                                  out.kng += c;
	}
	if (has_tempo()) {
		float z[HumanLike::NUM_TEMPO_FEATURES];
		HumanLike::extract_tempo_features(pos, z);
		for (int k = 0; k < HumanLike::NUM_TEMPO_FEATURES; ++k) {
			const double c = (double)g_w.w[(size_t)n + k] * (double)z[k];
			out.contrib[(size_t)n + k] = c;
			out.tmp += c;
		}
	}
}

// 直前の節点から寄与が動いた列を、動いた量の大きい順に topn 個。
std::string top_movers(const HumanLike::Layout& layout,
                       const Snapshot& before, const Snapshot& after, int topn) {
	const int n = layout.features();
	const int cols = (int)after.contrib.size();
	std::vector<int> order;
	order.reserve((size_t)cols);
	for (int i = 0; i < cols; ++i)
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
		const std::string name = i < n ? HumanLike::feature_v2_name(layout, i)
		                               : HumanLike::tempo_feature_name(i - n);
		if (k) out += ",";
		out += "{\"name\":" + json_quote(name)
		     + ",\"d\":" + num(after.contrib[(size_t)i] - before.contrib[(size_t)i]) + "}";
	}
	out += "]";
	return out;
}

// 一つの局面の五つの部分 (先手視点)。has_* が偽の部分は、モデルにその部分が無い。
struct Parts {
	double material = 0.0, tempo = 0.0, speed = 0.0, king = 0.0, activity = 0.0;
	bool   has_speed = false, has_king = false, has_activity = false;
};

// pos の五つの部分。left は pos の残り手数、speed_cols は speed_features の 8 列
// (pos の手番側から数えた並び)。速度の部分が要らないか、列が無いときは nullptr。
Parts compute_parts(Position& pos, float left, const int* speed_cols) {
	Parts p;
	int count[NUM_MATERIAL_COLUMNS];
	material_counts(pos, count);

	// 駒得は、駒の価値の表に枚数を掛けたもの (進行度の倍率の前)。
	double material = 0.0;
	for (int i = 0; i < NUM_MATERIAL_COLUMNS; ++i)
		material += (double)count[i] * (double)g_model.material_values()[i];
	material *= (double)g_model.curve(Model::MATERIAL, left);

	const double side = pos.side_to_move() == BLACK ? 1.0 : -1.0;
	const double tempo = side * (double)g_model.tempo_value() * (double)g_model.curve(Model::TEMPO, left);

	// 速度、玉の安全度、駒の働きは、それぞれ倍率を掛けたあとの値。
	double speed = 0.0, king = 0.0, activity = 0.0;
	if (g_nets.speed.valid() && speed_cols) {
		float x[NUM_SPEED], y[NUM_SPEED];
		for (int i = 0; i < NUM_SPEED; ++i) {
			x[i] = float(speed_cols[i]) * 0.25f;
			y[i] = -x[i];
		}
		const double raw = (double)(g_nets.speed.forward(x) - g_nets.speed.forward(y)) * 500.0;
		speed = raw * (double)g_model.curve(Model::SPEED, left);
		p.has_speed = true;
	}
	if (g_nets.king.valid() || g_nets.activity.valid()) {
		uint8_t board[NUM_BOARD];
		board_features(pos, board);
		if (g_nets.king.valid()) {
			std::vector<float> window(size_t(window_width(g_nets.king_radius)));
			king_window(board, 0, g_nets.king_radius, window.data());
			const float black = g_nets.king.forward(window.data());
			king_window(board, 1, g_nets.king_radius, window.data());
			const float white = g_nets.king.forward(window.data());
			king = (double)(black - white) * 500.0 * (double)g_model.curve(Model::KING, left);
			p.has_king = true;
		}
		if (g_nets.activity.valid()) {
			std::vector<float> facts, sign;
			const int n = piece_facts(board, facts, sign);
			double sum = 0.0;
			for (int i = 0; i < n; ++i)
				sum += (double)g_nets.activity.forward(&facts[size_t(i) * PIECE_FACTS]) * (double)sign[i];
			activity = sum * 100.0 * (double)g_model.curve(Model::ACTIVITY, left);
			p.has_activity = true;
		}
	}

	// 駒の枚数の差で説明できる部分を、速度・玉・駒の働きから引いて駒得に足す。
	// 合計は変わらない。駒得は「どの要素から見ても駒の価値」になり、ほかには駒の数では言えない分が残る。
	auto shift = [&](bool has, double& value, const float* table) {
		if (!has) return;
		double moved = 0.0;
		for (int i = 0; i < NUM_MATERIAL_COLUMNS; ++i)
			moved += (double)count[i] * (double)table[i];
		value    -= moved;
		material += moved;
	};
	shift(p.has_speed,    speed,    g_nets.moved_speed);
	shift(p.has_king,     king,     g_nets.moved_king);
	shift(p.has_activity, activity, g_nets.moved_activity);

	p.material = material;
	p.tempo    = tempo;
	p.speed    = speed;
	p.king     = king;
	p.activity = activity;
	return p;
}

std::string parts_json(const Parts& p) {
	std::string out = "\"material\":" + num(p.material) + ",\"tempo\":" + num(p.tempo);
	if (p.has_speed)    out += ",\"speed\":"    + num(p.speed);
	if (p.has_king)     out += ",\"king\":"     + num(p.king);
	if (p.has_activity) out += ",\"activity\":" + num(p.activity);
	return out;
}

// 一つの節点ぶんの JSON。move が MOVE_NONE なら根の局面。
void emit_node(Position& pos, const HumanLike::Layout& layout,
               int ply, Move move, int see, bool gives_check,
               const Snapshot* before, const Snapshot& now, int topn) {
	std::string line = "{\"ply\":" + std::to_string(ply);
	if (move != Move::none()) {
		line += ",\"move\":" + json_quote(to_usi_string(move))
		      + ",\"see\":" + std::to_string(see)
		      + ",\"gives_check\":" + (gives_check ? "true" : "false");
	}
	// 進行度のファイルを読んでいるときだけ。無いときの出力は今までと同じ。
	if (g_progress.loaded())
		line += ",\"moves_left\":" + num(g_progress.moves_left(pos));
	// 王手と短い詰みの 8 つの数。先手視点。速度の部分と、HceSpeed の出力で使うので一度だけ求める。
	int  speed_cols[NUM_SPEED];
	bool have_speed_cols = false;
	if ((model_ready() && g_nets.speed.valid()) || g_speed) {
		speed_features(pos, speed_cols);
		have_speed_cols = true;
	}
	// モデルを読んでいるときだけ。要素ごとの値で、先手視点。足し合わせるとモデルの評価値になる。
	if (model_ready()) {
		const float left = g_progress.moves_left(pos);
		const Parts here = compute_parts(pos, left, have_speed_cols ? speed_cols : nullptr);
		line += ",\"parts\":{" + parts_json(here) + "}";

		// 手番を渡した局面 (手番だけを入れ替えた同じ盤) の部分。詰めろや、受けが一つしかない
		// 局面を見るため。王手がかかっているときは手番を渡せないので null。
		// 速度の 8 列は、手番を渡すと「手番側」と「手番でない側」が入れ替わるだけなので、
		// 前半と後半を入れ替えて使う (詰みの探索をやり直さない)。
		if (pos.in_check()) {
			line += ",\"pass\":null";
		} else {
			int swapped[NUM_SPEED];
			if (have_speed_cols)
				for (int i = 0; i < NUM_SPEED; ++i)
					swapped[i] = speed_cols[(i + NUM_SPEED / 2) % NUM_SPEED];
			StateInfo st;
			pos.do_null_move(st);
			const float left_pass = g_progress.moves_left(pos);
			const Parts passed = compute_parts(pos, left_pass, have_speed_cols ? swapped : nullptr);
			pos.undo_null_move();
			line += ",\"pass\":{\"moves_left\":" + num(left_pass) + ",\"parts\":{" + parts_json(passed) + "}}";
		}
	}
	// 王手と短い詰みの 8 つの数。先手視点。重いので HceSpeed が真のときだけ。
	if (g_speed) {
		line += ",\"speed\":{";
		for (int i = 0; i < NUM_SPEED; ++i)
			line += std::string(i ? "," : "") + "\"" + kSpeedNames[i] + "\":" + std::to_string(speed_cols[i]);
		line += "}";
	}
	line += ",\"sfen\":" + json_quote(pos.sfen())
	      + ",\"black_pov\":" + num(clamp3000(now.mat + now.mob + now.kng + now.tmp + g_w.bias))
	      + ",\"material\":" + num(now.mat)
	      + ",\"mobility\":" + num(now.mob)
	      + ",\"king\":" + num(now.kng);
	// 手番の列がない重みでは tempo を出さない。そうした重みの出力は、
	// 手番の列を足す前と一字一句同じになる。
	if (has_tempo())
		line += ",\"tempo\":" + num(now.tmp);
	if (before) {
		line += ",\"d_material\":" + num(now.mat - before->mat)
		      + ",\"d_mobility\":" + num(now.mob - before->mob)
		      + ",\"d_king\":" + num(now.kng - before->kng);
		if (has_tempo())
			line += ",\"d_tempo\":" + num(now.tmp - before->tmp);
		line += ",\"top\":" + top_movers(layout, *before, now, topn);
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
	// 空なら要素に分けない。読めないときも分けず、理由を info string で知らせる。
	options.add("HceExplainFile", Option("", [](const Option& o) {
		const std::string path = (std::string)o;
		Hce::g_model = Hce::Model();
		Hce::g_nets  = Hce::Nets();
		if (path.empty())
			return std::nullopt;
		std::string error;
		Hce::Model loaded;
		if (loaded.load(path, error) && Hce::build_nets(loaded, error)) {
			Hce::g_model = loaded;
			sync_cout << "info string HceExplainFile: loaded " << path << sync_endl;
			Hce::warn_if_unpaired();
		} else {
			sync_cout << "info string HceExplainFile: " << error << sync_endl;
		}
		return std::nullopt;
	}));
	// 局面の升ごとの事実を、学習側と突き合わせるために 1 行で出す。explain の出力には影響しない。
	// 値は UCI の option として受けた SFEN。出力は "hce_board": [987 個の整数]。
	options.add("HceDumpBoard", Option("", [](const Option& o) {
		const std::string sfen = (std::string)o;
		if (sfen.empty())
			return std::nullopt;
		Position pos;
		StateInfo st;
		if (pos.set(sfen, &st).has_value()) {
			sync_cout << "info string HceDumpBoard: bad SFEN" << sync_endl;
			return std::nullopt;
		}
		uint8_t out[Hce::NUM_BOARD];
		Hce::board_features(pos, out);
		std::string line = "{\"hce_board\":[";
		for (int i = 0; i < Hce::NUM_BOARD; ++i)
			line += (i ? "," : "") + std::to_string(int(out[i]));
		line += "]}";
		sync_cout << line << sync_endl;
		return std::nullopt;
	}));
	options.add("HceSpeed", Option(false, [](const Option& o) {
		Hce::g_speed = (bool)o;
		return std::nullopt;
	}));
	// 空なら進行度を出さない。読めないときも出さず、理由を info string で知らせる。
	options.add("HceProgressFile", Option("", [](const Option& o) {
		const std::string path = (std::string)o;
		Hce::g_progress = Hce::Progress();
		if (path.empty())
			return std::nullopt;
		std::string error;
		Hce::Progress loaded;
		if (loaded.load(path, error)) {
			Hce::g_progress = loaded;
			sync_cout << "info string HceProgressFile: loaded " << path << sync_endl;
			Hce::warn_if_unpaired();
		} else {
			sync_cout << "info string HceProgressFile: " << error << sync_endl;
		}
		return std::nullopt;
	}));
}

void hce_explain(Position& pos, const std::vector<Move>& pv, int topn, bool header) {
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
	if (header) {
		sync_cout << "{\"route\":\"linear\""
		          << ",\"weights\":" << json_quote(g_w.source)
		          << ",\"variant\":" << json_quote(HumanLike::variant_name(layout.variant))
		          << ",\"pieces\":" << json_quote(HumanLike::pieces_name(layout.pieces))
		          << ",\"features\":" << (has_tempo() ? layout.with_tempo() : layout.features())
		          << ",\"phase\":" << HumanLike::game_phase(pos)
		          << ",\"pov\":\"black\""
		          << ",\"caveat\":" << json_quote(
		                 "項の絶対値は版ごとに配り方が違う。読んでよいのは同じ版の中での差分だけ。")
		          << "}" << sync_endl;
	}

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
