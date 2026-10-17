#ifndef EVAL_MANAKA_MANAKA_NET_H_INCLUDED
#define EVAL_MANAKA_MANAKA_NET_H_INCLUDED

// 評価関数ファイルから読んだネットワークと、接着側 (evaluate_manaka.cpp) の境目。
//
// 接着側が持つもの : Position から BoardView への変換 (manaka_board.h)、StateInfo の枠と旗、
//                   祖先の選び方、スレッドごとの Session の管理、pre から内部値への写像。
// ネットワーク側が持つもの : ファイルの読み込みと検査、アキュムレータの更新、pre (tanh の前の f32) の計算。
//
// 形式は nn.bin (SYMq16c と旧 SYMq16、manaka_net_q16.cpp) と、開発用の MNW1 (manaka_net_mnw.cpp)。
// 約束事は eval/manaka/API.md。

#include "../../config.h"

#if defined(EVAL_MANAKA)

#include <memory>
#include <string>

#include "core/board.h"
#include "manaka_accumulator.h"

namespace YaneuraOu {
namespace Eval {
namespace Manaka {

// pre (手番側から見た、tanh の前の出力) から内部単位への写像。
//
//   内部値 = trunc( clamp(pre, -atanh(c), +atanh(c)) * coef * unit )
//
// 教師の value_q は tanh(cp / coef) で、cp は USI のセンチポーン (coef は各シャードのフッターの
// eval_coef、1512.173)。unit は cp を YaneuraOu の内部単位 (PawnValue = 90 なので 0.9) に直す。
// atanh(tanh(pre)) = pre なので tanh も atanh も計算しない。c は 0.9999 で、manaka-usi の
// cp_from_value (value を ±0.9999 に切る) と同じ端で止める。
//
// SYMq16 系の cp は clamp(pre) * 1512.173 の切り捨てで、YaneuraOu の
// FV_SCALE は掛けない。ここで 0.9 を掛けて最後に一度だけ切り捨てる (cp を先に切り捨ててから 0.9 を
// 掛けない)。定数はファイルが自己記述しないので、ネットワークの種類ごとに固定の値を持つ。
//
// 変換は manaka_output_map.cpp に置く。その翻訳単位は -ffast-math を外してビルドされ、pre が NaN や無限大のとき
// 変換せずに false を返す。接着側の翻訳単位は -ffast-math で、isfinite を最適化で消されうるので、
// 有限性の判定と float から int への変換はここへ寄せる (呼び出し側は戻り値の bool だけを見る)。
class OutputMap {
public:
	OutputMap();
	OutputMap(double coef, double unit, double value_clamp);

	// pre が有限なら、整数 (0 に向かって切り捨て) を *out に書いて true を返す。有限でなければ false で *out は変えない。
	// pre は ±atanh(c) に切ってから掛けるので、結果は常に int に収まる。
	bool to_internal(float pre, int* out) const;

	double scale() const { return scale_; }
	double limit() const { return limit_; }

private:
	double scale_;
	double limit_;
};

// 1 スレッドが 1 つ持つ作業用の状態。重み (Network) は共有の読み取り専用で、Session だけが書く。
//
// どの関数も pre (手番側から見た tanh の前の f32) を返す。失敗したら false を返し、*pre は変えず、
// err に理由を入れる (err は nullptr でもよい)。
// 失敗した後の acc.q16 は壊れていてよい。呼び出し側が旗を false にしてから呼び、成功してから true にする。
class Session {
public:
	virtual ~Session() = default;

	// acc.q16 が評価に使われ、更新できるネットワークなら true。false のネットワーク (開発用の f32) は
	// rebuild と score のたびに盤から全計算し、acc には触らない。
	virtual bool incremental() const = 0;

	// board から acc を一から作り直して評価する。
	virtual bool rebuild(const ManakaCore::BoardView& board, Accumulator& acc, float* pre, std::string* err) = 0;

	// 直前の局面が使えないとき (未計算、または他のスレッドが作った) の入口。rebuild と同じ結果 (整数なのでビット単位で
	// 同じ) を返すが、Session が自分で持つ直近に計算した局面のアキュムレータのうち、トークンの差が小さいものが
	// あれば、そこからの差分で作る。Session は 1 スレッドのものなので、持っているのは自分のスレッドが作ったものだけ。
	// 既定は rebuild。force (compute_eval) の全計算はこちらではなく rebuild を使う。
	virtual bool rebuild_near(const ManakaCore::BoardView& board, Accumulator& acc, float* pre, std::string* err) {
		return rebuild(board, acc, pre, err);
	}

	// parent の acc.q16 (有効で、同じネットワークで作ったもの) を acc に写し、board との差で更新して評価する。
	// parent と acc は別のオブジェクト。parent には書かない。
	virtual bool update(const ManakaCore::BoardView& board, const Accumulator& parent, Accumulator& acc,
	                    float* pre, std::string* err) = 0;

	// acc.q16 がすでに board のトークンの集合になっているとき (null move の直後など)、
	// 更新せずに board.side_to_move から見た pre だけを求める。
	virtual bool score(const ManakaCore::BoardView& board, const Accumulator& acc, float* pre, std::string* err) = 0;
};

// ファイルの内容を検証した結果。
//   kVerified      ファイルの末尾のチェックサム (CRC-32C) を、本体の全バイトについて照合して一致した。
//   kUnverified    チェックサムの無い形式 (旧型の SYMq16)。壊れていても形式の検査を通れば受け入れる。
//   kNotApplicable チェックサムの概念が無い形式 (開発用の MNW1)。
enum class Verification { kVerified, kUnverified, kNotApplicable };

// 読み込み済みの重み。作ったあとは変わらず、スレッド間で共有する。同じ Network に読み直さない
// (読み直しは新しい Network を作り、世代を進める)。
class Network {
public:
	virtual ~Network() = default;

	virtual const OutputMap& output_map() const = 0;

	// 評価するスレッドごとに 1 つ作る。Network より先に壊してはいけない。
	virtual std::unique_ptr<Session> new_session() const = 0;

	// load_eval が info string に出す 1 行 (形の名前、重みの大きさ、checksum_verified など)。
	virtual std::string describe() const = 0;

	virtual Verification verification() const { return Verification::kNotApplicable; }
};

// path のファイルの先頭を見て形式を決めて読む。読めなければ nullptr を返し、*err に理由を入れる。
// 形式を取り違えて読まない。対応しない形式、壊れたファイルを、別の形式として救済しない。
// require_checksum が真なら、チェックサムで内容を検証できない形式 (旧型の SYMq16、開発用の MNW1) を拒否する。
std::unique_ptr<Network> load_network(const std::string& path, bool require_checksum, std::string* err);

// 形式ごとの読み込み口 (load_network が先頭を見て呼び分ける)。
std::unique_ptr<Network> load_network_q16(const std::string& path, bool require_checksum, std::string* err);
std::unique_ptr<Network> load_network_mnw(const std::string& path, bool require_checksum, std::string* err); // 開発用。-DMANAKA_DEV_MNW1 のときだけ読める

} // namespace Manaka
} // namespace Eval
} // namespace YaneuraOu

#endif // defined(EVAL_MANAKA)

#endif // EVAL_MANAKA_MANAKA_NET_H_INCLUDED
