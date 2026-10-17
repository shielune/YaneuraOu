#include "../../config.h"

#if defined(EVAL_MANAKA)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../../engine.h"
#include "../../types.h"
#include "../../position.h"
#include "../../evaluate.h"
#include "../../misc.h"
#include "../../movegen.h"
#include "../../usioption.h"
#include "evaluate_manaka.h"
#include "manaka_board.h"
#include "manaka_net.h"

// Manaka 評価関数の接着部分。Position を BoardView に写し、読み込んだネットワークに pre を計算させ、
// 内部単位の Value に直す。ファイルの読み込み、アキュムレータの更新、pre の計算は manaka_net.h の向こう側にある。
//
// 評価した値は StateInfo の manaka_accumulator.score に取っておき、同じ局面を探索が何度評価しても
// 一度しか計算しない。アキュムレータ (トークン集合の冪和と pair の和) は、直前の局面 (st->previous)
// が計算済みならそこから差分で更新し、そうでなければ盤から作り直す。
//
// 書き込むのは pos.state() の manaka_accumulator だけで、previous には読み取りしか行わない。
// 約束事は eval/manaka/API.md を見ること。

namespace YaneuraOu {
namespace Eval {

namespace {

// EvalDir と EvalFile の現在値。既定は NNUE と同じ eval と nn.bin。
std::string g_eval_dir  = "eval";
std::string g_eval_file = "nn.bin";

// USI オプション ManakaRequireChecksum。真なら、チェックサムで内容を検証できない形式 (旧型の SYMq16) を拒否する。
// 値が変わったら読み込み済みフラグを倒し、次の isready で読み直す。load_eval と add_options のコールバックだけが触る。
bool g_require_checksum = false;

// load_eval() が読み込み済みか。EvalDir と EvalFile が変わったら倒して、
// 次の isready で読み直させる (NNUE の eval_loaded と同じ作法)。load_eval と add_options の
// コールバックだけが触る。
bool g_loaded = false;

// 読み込み済みのネットワークとその世代。世代は 1 から始まり、0 は未読み込み。
// g_net と g_generation は g_mutex の下で一緒に更新する。評価のたびに読むのは g_generation だけ。
// 静的初期化の時点 (Position のコンストラクタが compute_eval を呼ぶ) で触っても安全なように、
// どれも定数初期化できる型にしてある。
std::mutex                             g_mutex;
std::shared_ptr<const Manaka::Network> g_net;
std::atomic<std::uint32_t>             g_generation{0};

// 評価できなかったときの info string を一度だけ出すための旗。
std::atomic<bool> g_reported_failure{false};

// スレッドごとの番号。1 から振り、再利用しない (アキュムレータの owner の印に使う)。
std::atomic<std::uint32_t> g_next_thread_id{1};

// 評価の経路ごとの回数。スレッドごとに数える (他のスレッドの書き込みと同じ行を使わない)。
struct PathCounts {
	std::uint64_t cached      = 0; // 取ってある score を返した
	std::uint64_t score_only  = 0; // アキュムレータは現局面のもので、評価だけした (null move の直後など)
	std::uint64_t updated     = 0; // 直前の局面から差分で更新した
	std::uint64_t rebuilt     = 0; // 盤から作り直した
	std::uint64_t failed      = 0; // 評価できなかった
	std::uint64_t foreign     = 0; // 有効な親だが、他のスレッドが作ったので使わなかった (root 以前の共有履歴、rootState のコピー)
};

// 生きているスレッドの PathCounts の一覧 (test manaka_stats が合計するため)。登録と削除と合計は同じ
// 排他の下で行う。数字そのものは他のスレッドが書き続けるので、合計は診断用の概算 (読み取りの競合を許す)。
struct CountsRegistry {
	std::mutex                     mutex;
	std::vector<const PathCounts*> items;
};
CountsRegistry& counts_registry() {
	static CountsRegistry registry; // 関数内 static: 静的初期化の順序に依らない
	return registry;
}

// スレッドごとの作業用の状態。世代が進んだら作り直す。
// Network を shared_ptr で持つので、読み直しで g_net が入れ替わっても、使っている間は消えない。
struct ThreadCache {
	const std::uint32_t                    id = g_next_thread_id.fetch_add(1);
	std::uint32_t                          generation = 0;
	std::shared_ptr<const Manaka::Network> net;
	std::unique_ptr<Manaka::Session>       session;
	Manaka::OutputMap                      map;
	PathCounts                             counts;

	ThreadCache() {
		CountsRegistry& r = counts_registry();
		std::lock_guard<std::mutex> lock(r.mutex);
		r.items.push_back(&counts);
	}
	~ThreadCache() {
		CountsRegistry& r = counts_registry();
		std::lock_guard<std::mutex> lock(r.mutex);
		r.items.erase(std::remove(r.items.begin(), r.items.end(), &counts), r.items.end());
	}
	ThreadCache(const ThreadCache&)            = delete;
	ThreadCache& operator=(const ThreadCache&) = delete;
};

thread_local ThreadCache t_cache;

// このスレッドの作業用の状態を最新にして返す。未読み込みなら nullptr。
ThreadCache* thread_cache() {
	ThreadCache& c = t_cache;
	if (c.generation != g_generation.load(std::memory_order_acquire))
	{
		std::shared_ptr<const Manaka::Network> net;
		std::uint32_t                          generation;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			net        = g_net;
			generation = g_generation.load(std::memory_order_relaxed);
		}
		c.session.reset(); // 古い Session を、古い Network が消える前に壊す。
		c.net        = std::move(net);
		c.generation = generation;
		c.map        = c.net ? c.net->output_map() : Manaka::OutputMap();
		if (c.net)
			c.session = c.net->new_session();
	}
	return (c.generation != 0 && c.session) ? &c : nullptr;
}

// pre から内部値を作る。pre が有限でなければ false (有限性の判定と変換は厳密な翻訳単位にある)。
bool to_value(const ThreadCache& c, float pre, Value* out) {
	int v;
	if (!c.map.to_internal(pre, &v))
		return false;
	if (v > VALUE_MAX_EVAL)
		v = VALUE_MAX_EVAL;
	else if (v < -VALUE_MAX_EVAL)
		v = -VALUE_MAX_EVAL;
	*out = static_cast<Value>(v);
	return true;
}

void report_failure(const std::string& err) {
	if (!g_reported_failure.exchange(true))
		sync_cout << "info string Manaka: the position cannot be evaluated (" << err
		          << "); the value is 0 (reported once)" << sync_endl;
}

// このアキュムレータが現在の重みで作った有効なものか。
inline bool valid_accumulation(const Manaka::Accumulator& acc, std::uint32_t generation) {
	return acc.computed_accumulation && acc.weights_gen == generation;
}

// 差分更新の親にしてよいか。有効で、かつ自分のスレッドが作ったものだけ。
// root 以前の StateInfo は全スレッドで共有の読み取り専用で、rootState (setupStates->back() の丸ごとのコピー) にも
// 他のスレッドの印が付いている。共有履歴を親にしない。作成者でない親は作り直しに回す。
inline bool usable_parent(const Manaka::Accumulator& acc, const ThreadCache& c) {
	return valid_accumulation(acc, c.generation) && acc.owner == c.id;
}

// 局面の値を、取ってあればそれを返し、なければ計算して取っておく。
//   force = true : 取ってある値も直前の局面も使わず、盤から作り直す (compute_eval)。
// 未読み込みや、評価できない局面のときは VALUE_ZERO を返し、旗は倒れたままにする。
Value evaluate_and_keep(const Position& pos, bool force) {
	if (g_generation.load(std::memory_order_acquire) == 0)
		return VALUE_ZERO;
	ThreadCache* c = thread_cache();
	if (c == nullptr)
		return VALUE_ZERO;

	StateInfo* st  = pos.state();
	auto&      acc = st->manaka_accumulator;

	if (!force && acc.computed_score && acc.weights_gen == c->generation)
	{
		c->counts.cached++;
		return acc.score;
	}

	ManakaCore::BoardView board;
	if (!Manaka::make_board_view(pos, &board))
	{
		c->counts.failed++;
		report_failure("a piece that Manaka has no token for");
		return VALUE_ZERO;
	}

	Manaka::Session& session = *c->session;
	float            pre     = 0;
	std::string      err;
	bool             ok;

	if (!session.incremental())
	{
		// 開発用の f32。アキュムレータは使わず、毎回盤から全計算する。
		ok = session.rebuild(board, acc, &pre, &err);
		c->counts.rebuilt += ok;
	}
	else if (!force && valid_accumulation(acc, c->generation))
	{
		// do_null_move() はトークン集合を引き継ぎ、score の旗だけを倒す。更新せずに評価だけする。
		ok = session.score(board, acc, &pre, &err);
		c->counts.score_only += ok;
	}
	else
	{
		// 更新の途中で失敗しても、そろっていない状態を有効と見なさない。
		acc.computed_accumulation = false;

		// 直前の局面が現在の重みで計算済みなら、そこからの差分で更新する。previous には書かない。
		// 更新は、元になるアキュムレータが有効でさえあれば、どの局面のものからでも正しい (core が自分の
		// トークン一覧と board の差を取る)。だから直前の局面が本当の親かどうかには正しさが依らない。
		// 遡って探さず、計算済みでなければ作り直す (未計算の祖先を再帰して計算しない)。
		const StateInfo* parent = force ? nullptr : st->previous;
		if (parent != nullptr && usable_parent(parent->manaka_accumulator, *c))
		{
			ok = session.update(board, parent->manaka_accumulator, acc, &pre, &err);
			c->counts.updated += ok;
		}
		else
		{
			if (parent != nullptr && valid_accumulation(parent->manaka_accumulator, c->generation))
				c->counts.foreign++;
			// force は取ってある局面を一切使わない全計算。そうでなければ Session が持つ直近の局面からの差分も使う。
			ok = force ? session.rebuild(board, acc, &pre, &err) : session.rebuild_near(board, acc, &pre, &err);
			c->counts.rebuilt += ok;
		}
		if (ok)
		{
			acc.computed_accumulation = true;
			acc.owner                 = c->id;
		}
	}

	if (!ok)
	{
		acc.computed_accumulation = false;
		acc.computed_score        = false;
		c->counts.failed++;
		report_failure(err);
		return VALUE_ZERO;
	}

#ifdef MANAKA_VERIFY
	// 検証用の旗 (開発ビルドだけ): 評価のたびに、盤からの全計算 (rebuild、直近の局面を使わない) とアキュムレータの全要素と
	// pre のビットを突き合わせる。1 度でも違えば止まる。
	if (session.incremental())
	{
		static std::atomic<std::uint64_t> checked{0};
		static std::atomic<bool>          registered{false};
		if (!registered.exchange(true))
			std::atexit([] { std::fprintf(stderr, "MANAKA_VERIFY: %llu evaluations matched a full rebuild bit for bit\n", (unsigned long long)checked.load()); });
		Manaka::Accumulator scratch;
		float               pre2 = 0;
		std::string         err2;
		const bool          ok2  = session.rebuild(board, scratch, &pre2, &err2);
		const auto&         a = acc.q16;
		const auto&         b = scratch.q16;
		if (!ok2 || std::memcmp(&pre2, &pre, 4) != 0 || !(a.p1 == b.p1 && a.p2 == b.p2 && a.p3 == b.p3 && a.pair == b.pair && a.tokens == b.tokens))
		{
			std::fprintf(stderr, "MANAKA_VERIFY: MISMATCH pre %.9g vs full %.9g sfen %s\n", (double)pre, (double)pre2, pos.sfen().c_str());
			std::abort();
		}
		checked++;
	}
#endif
	Value v;
	if (!to_value(*c, pre, &v))
	{
		acc.computed_accumulation = false;
		acc.computed_score        = false;
		c->counts.failed++;
		report_failure("non-finite output");
		return VALUE_ZERO;
	}
	acc.score          = v;
	acc.weights_gen    = c->generation;
	acc.computed_score = true;
	return v;
}

// pre だけを、アキュムレータにも StateInfo にも触らずに盤から求める (trace と検査用)。
bool full_pre_scratch(ThreadCache& c, const Position& pos, float* pre, std::string* err) {
	ManakaCore::BoardView board;
	if (!Manaka::make_board_view(pos, &board))
	{
		if (err)
			*err = "a piece that Manaka has no token for";
		return false;
	}
	Manaka::Accumulator scratch;
	return c.session->rebuild(board, scratch, pre, err);
}

} // namespace

void add_options(OptionsMap& options, ThreadPool&) {

	// 評価関数フォルダ。相対なら実行ファイルのあるフォルダ基準で解決する (NNUE と同じ)。
	options.add("EvalDir", Option(g_eval_dir.c_str(), [](const Option& o) {
		const std::string dir = (std::string)o;
		if (dir != g_eval_dir)
		{
			g_eval_dir = dir;
			g_loaded   = false;
		}
		return std::nullopt;
	}));

	// 真なら、チェックサム (CRC-32C) の無い旧型の SYMq16 を拒否する (Error! を出して終了する)。
	// 既定は偽: 旧型は読めるが、内容は検証されない旨の警告を読み込みごとに一度出す。
	options.add("ManakaRequireChecksum", Option(g_require_checksum, [](const Option& o) {
		const bool require = (bool)o;
		if (require != g_require_checksum)
		{
			g_require_checksum = require;
			g_loaded           = false;
		}
		return std::nullopt;
	}));

	// 評価関数ファイル名。NNUE は WASM の時だけ持つが、Manaka は常に持つ。
	// 絶対パスなら EvalDir に関係なくその指定が使われる (Path::Combine の作法)。
	options.add("EvalFile", Option(g_eval_file.c_str(), [](const Option& o) {
		const std::string file = (std::string)o;
		if (file != g_eval_file)
		{
			g_eval_file = file;
			g_loaded    = false;
		}
		return std::nullopt;
	}));
}

// "isready" のたびに呼ばれる。読めなかったら NNUE と同じく終了する。
// 読めないファイルを 0 の評価で代替しない (評価が 0 のまま探索が動くのは、読めないより悪い)。
void load_eval() {
	if (g_loaded)
		return;

	const std::string path = Path::Combine(Path::Combine(Directory::GetBinaryFolder(), g_eval_dir), g_eval_file);
	sync_cout << "info string loading eval file : " << path << sync_endl;

	std::string                      err;
	std::unique_ptr<Manaka::Network> net = Manaka::load_network(path, g_require_checksum, &err);
	if (!net)
	{
		sync_cout << "Error! : failed to read " << path << " : " << err << sync_endl;
		Tools::exit();
		return;
	}
	sync_cout << "info string Manaka: " << net->describe() << sync_endl;
	if (net->verification() == Manaka::Verification::kUnverified)
		sync_cout << "info string Warning: nn.bin has no checksum (SYMq16); contents are not verified" << sync_endl;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_net = std::move(net);
		g_generation.store(g_generation.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	}
	g_reported_failure.store(false);
	g_loaded = true;
}

// 全計算。Position::set() から呼ばれる。取ってある値も直前の局面も使わず、盤から作り直す。
// 重みの読み込み前に呼ばれたら 0 を返し、旗には触らない。
Value compute_eval(const Position& pos) {
	return evaluate_and_keep(pos, true);
}

// 次の評価のために差分更新だけ進める入口。この fork の探索は USE_LAZY_EVALUATE を定義しないので
// 呼ばれない。評価の側が必要なときに直前の局面から更新するので、ここでは何もしない。
void evaluate_with_no_return(const Position&) {
}

// 評価値の内訳表示 (デバッグ用)。
void print_eval_stat(Position& pos) {
	std::cout << "--- EVAL STAT: EVAL_MANAKA" << std::endl;
	Manaka::trace(pos);
}

// 評価関数本体。手番側から見た内部値を返す。
Value evaluate(const Position& pos) {
	return evaluate_and_keep(pos, false);
}

namespace Manaka {

void trace(const Position& pos) {
	ThreadCache* c = thread_cache();
	if (c == nullptr)
	{
		sync_cout << "info string Manaka: no eval file is loaded" << sync_endl;
		return;
	}
	float       pre;
	std::string err;
	if (!full_pre_scratch(*c, pos, &pre, &err))
	{
		sync_cout << "info string Manaka: the position cannot be evaluated (" << err << ")" << sync_endl;
		return;
	}
	Value internal;
	if (!to_value(*c, pre, &internal))
	{
		sync_cout << "info string Manaka: the position cannot be evaluated (non-finite output)" << sync_endl;
		return;
	}
	char text[48];
	std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(pre));
	sync_cout << "manaka pre " << text << " internal " << static_cast<int>(internal) << sync_endl;
}

namespace {

// 局面を固定の乱数で歩き、各局面で次を確かめる。
//   1. evaluate が返す値が、同じ SFEN から作り直した Position の全計算と一致する。
//   2. 差分で更新したアキュムレータ (p1、p2、p3、pair、トークン) が、作り直したものとビット単位で一致する。
//   3. 先後を入れ替えて 180 度回した局面 (flipped_sfen) の pre が一致する。整数の経路ではビット単位で、
//      f32 の経路では足す順が逆になるので pre の差が 1e-4 以内、内部値の差が 1 以内で合格とする。
//   4. 同じ StateInfo を使い回す兄弟手、null move、数手を評価せずに進めてから評価する経路 (作り直しになる)、
//      王手中を含める。
//   5. 最後に重みを読み直し、古い世代のアキュムレータを使わずに作り直して、同じ値になる。
// 経路の回数 (更新、作り直し、評価だけ、取ってある値) も出す。作り直しだけで合格するのを防ぐため、
// 差分更新が 1 度も通らなかったら失敗とする。
struct WalkStats {
	std::uint64_t positions       = 0;
	std::uint64_t value_mismatch  = 0;
	std::uint64_t state_mismatch  = 0;
	std::uint64_t flip_mismatch   = 0;
	std::uint64_t in_check        = 0;
	std::uint64_t unevaluable     = 0;
	std::uint64_t flip_off_by_one = 0; // 反転で内部値が 1 だけずれた局面 (f32 の経路のみ)
	double        max_flip_dpre   = 0; // 反転で pre が動いた幅の最大
	PathCounts    paths;               // 検査の対象にした evaluate の経路だけ (作り直した Position の分を含めない)
};

bool same_state(const ManakaCore::Q16Accumulator& a, const ManakaCore::Q16Accumulator& b) {
	return a.p1 == b.p1 && a.p2 == b.p2 && a.p3 == b.p3 && a.pair == b.pair && a.tokens == b.tokens
	    && a.network == b.network;
}

void check_position(const Position& pos, WalkStats& stats, const char* what) {
	stats.positions++;
	if (pos.checkers())
		stats.in_check++;

	ThreadCache*     c      = thread_cache();
	const PathCounts before = c->counts;
	const Value      inc    = Eval::evaluate(pos);
	const PathCounts after  = c->counts;
	stats.paths.cached     += after.cached - before.cached;
	stats.paths.score_only += after.score_only - before.score_only;
	stats.paths.updated    += after.updated - before.updated;
	stats.paths.rebuilt    += after.rebuilt - before.rebuilt;
	stats.paths.failed     += after.failed - before.failed;
	stats.paths.foreign    += after.foreign - before.foreign;
	const auto& inc_acc = pos.state()->manaka_accumulator;

	// 作り直した Position。set() が compute_eval で盤から全計算する。
	Position  fresh;
	StateInfo fresh_si;
	if (fresh.set(pos.sfen(), &fresh_si))
	{
		stats.unevaluable++;
		return;
	}
	const auto& fresh_acc = fresh_si.manaka_accumulator;
	if (!fresh_acc.computed_score)
	{
		stats.unevaluable++;
		return;
	}

	if (inc != fresh_acc.score)
	{
		stats.value_mismatch++;
		sync_cout << "test manaka_walk: VALUE MISMATCH (" << what << ") evaluate " << inc << " fresh " << fresh_acc.score
		          << " sfen " << pos.sfen() << sync_endl;
	}
	if (c->session->incremental() && !same_state(inc_acc.q16, fresh_acc.q16))
	{
		stats.state_mismatch++;
		sync_cout << "test manaka_walk: STATE MISMATCH (" << what << ") sfen " << pos.sfen() << sync_endl;
	}

	// 先後を入れ替えて 180 度回した局面。トークンの集合は同じで、整数の和は足す順に依らない。
	float pre = 0, flipped_pre = 0;
	Position  flipped;
	StateInfo flipped_si;
	if (!flipped.set(pos.flipped_sfen(), &flipped_si) && full_pre_scratch(*c, pos, &pre, nullptr)
	    && full_pre_scratch(*c, flipped, &flipped_pre, nullptr))
	{
		const double dpre = std::fabs(static_cast<double>(flipped_pre) - static_cast<double>(pre));
		stats.max_flip_dpre = std::max(stats.max_flip_dpre, dpre);
		Value v0 = VALUE_ZERO, v1 = VALUE_ZERO;
		to_value(*c, pre, &v0);
		to_value(*c, flipped_pre, &v1);
		const int dv = std::abs(static_cast<int>(v1) - static_cast<int>(v0));
		stats.flip_off_by_one += (dv == 1);
		const bool exact = c->session->incremental();
		if (exact ? (flipped_pre != pre) : (dv > 1 || dpre > 1e-4))
		{
			stats.flip_mismatch++;
			sync_cout << "test manaka_walk: FLIP MISMATCH (" << what << ") pre " << pre << " flipped " << flipped_pre
			          << " sfen " << pos.sfen() << sync_endl;
		}
	}
}

} // namespace

void test_walk(IEngine& engine, std::istringstream& is) {
	std::uint64_t games = 20, plies = 120, seed = 1;
	std::string   token;
	while (is >> token)
	{
		if (token == "games")
			is >> games;
		else if (token == "plies")
			is >> plies;
		else if (token == "seed")
			is >> seed;
	}
	if (seed == 0)
		seed = 1;

	// 重みの読み込み (isready 相当)。
	engine.isready();
	ThreadCache* c = thread_cache();
	if (c == nullptr)
	{
		sync_cout << "test manaka_walk: no eval file is loaded" << sync_endl;
		return;
	}
	PRNG      prng(seed);
	WalkStats stats;

	for (std::uint64_t g = 0; g < games; ++g)
	{
		StateListPtr states(new StateList(1));
		Position     pos;
		pos.set_hirate(&states->back());

		for (std::uint64_t ply = 0; ply < plies; ++ply)
		{
			// 合法手がなければ終局。
			std::vector<Move> moves;
			for (auto m : MoveList<LEGAL_ALL>(pos))
				moves.push_back(m);
			if (moves.empty())
				break;

			// 兄弟手: 同じ StateInfo を使い回して、数手を指しては戻す。
			// do_move の冒頭で旗を倒さないと、前の手の残りの値を返す。
			{
				StateInfo    sibling;
				const size_t tries = std::min<size_t>(moves.size(), 4);
				for (size_t k = 0; k < tries; ++k)
				{
					const Move m = moves[prng.rand(moves.size())];
					pos.do_move(m, sibling);
					check_position(pos, stats, "sibling");
					pos.undo_move(m);
				}
			}

			// null move (王手中は指せない)。
			if (!pos.checkers())
			{
				StateInfo nullst;
				pos.do_null_move(nullst);
				check_position(pos, stats, "null");
				pos.undo_null_move();
			}

			// 本筋。4 回に 1 回は、1 から 3 手を評価せずに進めてから評価する (直前の局面が未計算の経路)。
			const std::uint64_t skip = prng.rand(4) == 0 ? 1 + prng.rand(3) : 0;
			for (std::uint64_t k = 0; k <= skip; ++k)
			{
				std::vector<Move> legal;
				for (auto m : MoveList<LEGAL_ALL>(pos))
					legal.push_back(m);
				if (legal.empty())
					break;
				const Move m = legal[prng.rand(legal.size())];
				states->emplace_back();
				pos.do_move(m, states->back());
				if (k > 0)
					++ply; // 評価せずに進めた分も手数に数える
			}
			check_position(pos, stats, skip ? "skip" : "main");
		}
	}

	// 重みの読み直し。旗は触らずに残し (rootState の丸ごとのコピーと同じく、旗が真のまま世代だけが古くなる)、
	// 古い世代の値とアキュムレータを使わずに作り直すことを、経路の回数で確かめる。値は同じファイルを
	// 読み直すので変わらない。値だけでは世代の確認を外しても気付けないので、回数を見る。
	bool reload_ok = true;
	{
		StateListPtr states(new StateList(1));
		Position     pos;
		pos.set_hirate(&states->back());
		for (int k = 0; k < 8; ++k)
		{
			std::vector<Move> legal;
			for (auto m : MoveList<LEGAL_ALL>(pos))
				legal.push_back(m);
			states->emplace_back();
			pos.do_move(legal[prng.rand(legal.size())], states->back());
		}
		const Value value_before = Eval::evaluate(pos); // 世代 g1 で有効
		const auto  flags_before = pos.state()->manaka_accumulator;

		// (1) 読み直したあと、同じ局面を評価する。取ってある score を返してはならず、作り直す。
		g_loaded = false;
		engine.isready();
		const PathCounts c1 = thread_cache()->counts;
		const Value      value1 = Eval::evaluate(pos);
		const PathCounts c2 = thread_cache()->counts;
		// 全計算だけの開発用の経路 (f32) は accumulation の旗を立てない。
		const bool step1 = value1 == value_before && flags_before.computed_score
		                && (!thread_cache()->session->incremental() || flags_before.computed_accumulation)
		                && c2.cached == c1.cached && c2.score_only == c1.score_only && c2.rebuilt == c1.rebuilt + 1
		                && pos.state()->manaka_accumulator.weights_gen != flags_before.weights_gen;

		// (2) もう一度読み直し、親を評価しないまま子を評価する。親のアキュムレータは古い世代なので、
		//     そこから更新してはならず、作り直す。
		g_loaded = false;
		engine.isready();
		std::vector<Move> legal;
		for (auto m : MoveList<LEGAL_ALL>(pos))
			legal.push_back(m);
		states->emplace_back();
		pos.do_move(legal[prng.rand(legal.size())], states->back());
		const PathCounts c3 = thread_cache()->counts;
		const Value      child = Eval::evaluate(pos);
		const PathCounts c4 = thread_cache()->counts;
		Position  fresh;
		StateInfo fresh_si;
		const bool fresh_ok = !fresh.set(pos.sfen(), &fresh_si) && fresh_si.manaka_accumulator.computed_score;
		const bool step2 = fresh_ok && child == fresh_si.manaka_accumulator.score
		                && c4.updated == c3.updated && c4.rebuilt == c3.rebuilt + 1;

		reload_ok = step1 && step2;
		if (!reload_ok)
			sync_cout << "test manaka_walk: RELOAD FAILED step1 " << step1 << " step2 " << step2 << sync_endl;
	}

	// 親の作成者。有効で世代も合う親でも、他のスレッドが作ったもの (root 以前の共有履歴、rootState のコピー) は
	// 差分更新に使わず、作り直す。自分が作った親は更新に使う。値はどちらでも同じなので、経路の回数で確かめる。
	bool parent_ok = true;
	{
		ThreadCache* cc = thread_cache();
		if (cc != nullptr && cc->session->incremental())
		{
			StateListPtr states(new StateList(1));
			Position     pos;
			pos.set_hirate(&states->back()); // 自分の評価で、有効で自分が作った親になる
			states->emplace_back();
			std::vector<Move> legal;
			for (auto m : MoveList<LEGAL_ALL>(pos))
				legal.push_back(m);
			const Move m = legal[prng.rand(legal.size())];
			pos.do_move(m, states->back());
			const PathCounts p0 = cc->counts;
			const Value      own = Eval::evaluate(pos);
			const PathCounts p1 = cc->counts;
			const bool       own_ok = p1.updated == p0.updated + 1 && p1.foreign == p0.foreign;

			// 同じ局面を、親だけ他のスレッドの印にして評価し直す。
			states->back().previous->manaka_accumulator.owner = cc->id + 1;
			states->back().manaka_accumulator.computed_score        = false;
			states->back().manaka_accumulator.computed_accumulation = false;
			const PathCounts p2 = cc->counts;
			const Value      foreign_value = Eval::evaluate(pos);
			const PathCounts p3 = cc->counts;
			const bool foreign_ok = foreign_value == own && p3.updated == p2.updated && p3.rebuilt == p2.rebuilt + 1
			                     && p3.foreign == p2.foreign + 1 && states->back().manaka_accumulator.owner == cc->id;
			parent_ok = own_ok && foreign_ok;
			if (!parent_ok)
				sync_cout << "test manaka_walk: PARENT OWNER FAILED own " << own_ok << " foreign " << foreign_ok << sync_endl;
		}
	}

	// 出力の写像の境界。pre が NaN や無限大のとき、変換せずに失敗する (fast-math の翻訳単位でも消えない)。
	// 巨大な有限値は ±atanh(0.9999) に切られて、int に収まる。
	bool map_ok = true;
	{
		auto from_bits = [](std::uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
		const Manaka::OutputMap& map = thread_cache()->map;
		int v = 12345;
		const float nonfinite[] = { from_bits(0x7fc00000u), from_bits(0xffc00000u), from_bits(0x7f800000u), from_bits(0xff800000u),
		                            from_bits(0x7f800001u) /* signaling NaN */ };
		for (float f : nonfinite)
			map_ok = map_ok && !map.to_internal(f, &v) && v == 12345;
		const float finite[]   = { 0.0f, -0.0f, 1e-30f, 4.9517f, -4.9517f, 5.0f, 100.0f, 3.4028235e38f, -3.4028235e38f };
		const int   expected[] = { 0, 0, 0, 6739, -6739, 6739, 6739, 6739, -6739 }; // 4.9517 * 1360.9557 = 6739.0..、頭打ちも 6739
		for (size_t i = 0; i < sizeof(finite) / sizeof(finite[0]); ++i)
		{
			const bool good = map.to_internal(finite[i], &v) && std::abs(v - expected[i]) <= 1
			               && (i < 3 ? v == 0 : true);
			map_ok = map_ok && good;
			if (!good)
				sync_cout << "test manaka_walk: MAP FAILED pre " << finite[i] << " -> " << v << sync_endl;
		}
		// 0 に向かう切り捨て: 正負で対称。
		int pos_v = 0, neg_v = 0;
		map_ok = map_ok && map.to_internal(0.123456f, &pos_v) && map.to_internal(-0.123456f, &neg_v) && pos_v == -neg_v;
	}

	c = thread_cache();

	sync_cout << "test manaka_walk: games " << games << " plies " << plies << " seed " << seed
	          << " positions " << stats.positions << " in_check " << stats.in_check << sync_endl;
	sync_cout << "test manaka_walk: value_mismatch " << stats.value_mismatch << " state_mismatch " << stats.state_mismatch
	          << " flip_mismatch " << stats.flip_mismatch << " flip_off_by_one " << stats.flip_off_by_one
	          << " max_flip_dpre " << stats.max_flip_dpre << " unevaluable " << stats.unevaluable
	          << " reload " << (reload_ok ? "ok" : "FAILED") << sync_endl;
	sync_cout << "test manaka_walk: paths updated " << stats.paths.updated << " rebuilt " << stats.paths.rebuilt
	          << " score_only " << stats.paths.score_only << " cached " << stats.paths.cached
	          << " failed " << stats.paths.failed << sync_endl;

	// 作り直しだけで合格しないように、増分の経路 (更新、評価だけ) が通っていることも条件にする。
	const bool paths_ok = !c->session->incremental() || (stats.paths.updated > 0 && stats.paths.score_only > 0);
	sync_cout << "test manaka_walk: parent_owner " << (parent_ok ? "ok" : "FAILED") << " output_map " << (map_ok ? "ok" : "FAILED")
	          << " foreign_parent_in_walk " << stats.paths.foreign << sync_endl;
	const bool ok = stats.value_mismatch == 0 && stats.state_mismatch == 0 && stats.flip_mismatch == 0
	             && stats.unevaluable == 0 && reload_ok && parent_ok && map_ok && paths_ok && stats.paths.failed == 0;
	sync_cout << (ok ? "test manaka_walk: OK" : "test manaka_walk: FAILED") << sync_endl;
}

// "test manaka_bench stats [reset]" (共有のディスパッチに入口が無いので bench の引数として受ける)
// これまでに全スレッドが評価した経路の回数の合計 (探索のあとに呼ぶ)。reset を付けるとゼロに戻す。
// 合計は他のスレッドが動いている間の読み取りを含む概算。探索が終わっていれば正確。
void test_stats(IEngine&, std::istringstream& is) {
	std::string option;
	is >> option;
	PathCounts total;
	std::size_t threads;
	{
		CountsRegistry&             r = counts_registry();
		std::lock_guard<std::mutex> lock(r.mutex);
		threads = r.items.size();
		for (const PathCounts* p : r.items)
		{
			total.cached     += p->cached;
			total.score_only += p->score_only;
			total.updated    += p->updated;
			total.rebuilt    += p->rebuilt;
			total.failed     += p->failed;
			total.foreign    += p->foreign;
			if (option == "reset")
				*const_cast<PathCounts*>(p) = PathCounts();
		}
	}
	const std::uint64_t computed = total.score_only + total.updated + total.rebuilt;
	char                text[96] = "";
	if (computed)
		std::snprintf(text, sizeof(text), " updated_share %.4f rebuilt_share %.4f",
		              double(total.updated) / double(computed), double(total.rebuilt) / double(computed));
	sync_cout << "test manaka_stats: threads " << threads << " cached " << total.cached << " score_only " << total.score_only
	          << " updated " << total.updated << " rebuilt " << total.rebuilt << " failed " << total.failed << " foreign_parent " << total.foreign << text
	          << (option == "reset" ? " (reset)" : "") << sync_endl;
}

// "test manaka_bench <局面ファイル> <回数> [warm|cold]"
//   cold : 局面ごとに、直前の局面を使わず盤から作り直す評価の時間。
//   warm : 局面から 1 手 (合法手を固定の乱数で選ぶ) 指した子を、親のアキュムレータから差分で更新する評価の時間。
// 評価の呼び出し 1 回ごとの時間を計り、平均、中央値、99 パーセンタイルを出す。局面の読み込みと
// 手を指す時間は含めない。局面は先頭から最大 2000 個を使い、回数に達するまで繰り返し回る。
void test_bench(IEngine& engine, std::istringstream& is) {
	std::string   path;
	std::uint64_t count = 0;
	std::string   mode  = "warm";
	is >> path;
	if (path == "stats") // 共有のディスパッチに入口が無いので、ここから呼ぶ。
	{
		test_stats(engine, is);
		return;
	}
	is >> count;
	if (!(is >> mode))
		mode = "warm";
	if (path.empty() || count == 0 || (mode != "warm" && mode != "cold"))
	{
		sync_cout << "usage: test manaka_bench <sfen file> <count> [warm|cold]" << sync_endl;
		return;
	}

	std::ifstream file(path);
	if (!file.is_open())
	{
		sync_cout << "test manaka_bench: cannot open " << path << sync_endl;
		return;
	}
	std::vector<std::string> sfens;
	for (std::string line; sfens.size() < 2000 && std::getline(file, line);)
	{
		if (line.rfind("sfen ", 0) == 0)
			line = line.substr(5);
		if (!line.empty() && line[0] != '#')
			sfens.push_back(line);
	}
	if (sfens.empty())
	{
		sync_cout << "test manaka_bench: no positions in " << path << sync_endl;
		return;
	}

	engine.isready();
	ThreadCache* c = thread_cache();
	if (c == nullptr)
	{
		sync_cout << "test manaka_bench: no eval file is loaded" << sync_endl;
		return;
	}

	std::deque<StateInfo>                  states;
	std::vector<std::unique_ptr<Position>> positions;
	for (const std::string& sfen : sfens)
	{
		states.emplace_back();
		std::unique_ptr<Position> p(new Position());
		if (p->set(sfen, &states.back()))
		{
			states.pop_back();
			continue;
		}
		positions.push_back(std::move(p)); // set() の compute_eval で、アキュムレータは有効になっている
	}
	if (positions.empty())
	{
		sync_cout << "test manaka_bench: no position could be set" << sync_endl;
		return;
	}

	PRNG                prng(1);
	std::vector<double> micros;
	micros.reserve(count);
	const PathCounts before      = c->counts;
	std::uint64_t    zero_values = 0;

	for (std::uint64_t i = 0; i < count; ++i)
	{
		Position& pos = *positions[i % positions.size()];

		if (mode == "cold")
		{
			auto& acc                 = pos.state()->manaka_accumulator;
			acc.computed_accumulation = false;
			acc.computed_score        = false;
			const auto  t0            = std::chrono::steady_clock::now();
			const Value v             = Eval::evaluate(pos);
			const auto  t1            = std::chrono::steady_clock::now();
			zero_values += (v == VALUE_ZERO);
			micros.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
		}
		else
		{
			std::vector<Move> legal;
			for (auto m : MoveList<LEGAL_ALL>(pos))
				legal.push_back(m);
			if (legal.empty())
				continue;
			const Move m = legal[prng.rand(legal.size())];
			StateInfo  child;
			pos.do_move(m, child);
			const auto  t0 = std::chrono::steady_clock::now();
			const Value v  = Eval::evaluate(pos);
			const auto  t1 = std::chrono::steady_clock::now();
			pos.undo_move(m);
			zero_values += (v == VALUE_ZERO);
			micros.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
		}
	}

	if (micros.empty())
	{
		sync_cout << "test manaka_bench: nothing was measured" << sync_endl;
		return;
	}
	double sum = 0;
	for (double u : micros)
		sum += u;
	std::sort(micros.begin(), micros.end());
	const PathCounts after = c->counts;
	char             text[256];
	std::snprintf(text, sizeof(text), "mean %.3f us median %.3f us p99 %.3f us min %.3f us max %.3f us",
	              sum / micros.size(), micros[micros.size() / 2], micros[size_t(0.99 * (micros.size() - 1))],
	              micros.front(), micros.back());
	sync_cout << "test manaka_bench: mode " << mode << " positions " << positions.size() << " evaluations "
	          << micros.size() << " " << text << sync_endl;
	sync_cout << "test manaka_bench: paths updated " << (after.updated - before.updated) << " rebuilt "
	          << (after.rebuilt - before.rebuilt) << " score_only " << (after.score_only - before.score_only)
	          << " cached " << (after.cached - before.cached) << " failed " << (after.failed - before.failed)
	          << " zero_values " << zero_values << sync_endl;
}

} // namespace Manaka

} // namespace Eval
} // namespace YaneuraOu

#endif // EVAL_MANAKA
