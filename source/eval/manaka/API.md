# Manaka 接着 API

評価の接着層 (source/eval/manaka/ の evaluate_manaka.*、manaka_*.h、manaka_net_*.cpp) と、
純 C++17 の core (source/eval/manaka/core/) の境界。
形式は nn.bin で、ModelType は 2 種ある (下の「ファイル形式とチェックサム」)。
対応する構造は pair16 の Tokens 空間、d128、order3、h512 と h512_h2256 の 2 種だけ。
他の構造は読み込み時に拒否する。利用方法は [Manaka エディション](../../../docs/manaka.md) を参照。

## 組み込み

config.h でエディションを選び、position.h と position.cpp でアキュムレータを管理する。
engine/yaneuraou-engine/yaneuraou-search.cpp の trace_eval と testcmd/normal_test_cmd.cpp から
検査の入口を呼ぶ。eval/nnue/evaluate_nnue.cpp は Manaka 形式を拒否する。
Makefile の MANAKA ブロックで core の翻訳単位と浮動小数のフラグを設定する。

core/ は YaneuraOu の型 (Position、Piece、Value、StateInfo、OptionsMap) に依存しない。
接着側が Position から BoardView を作って core に渡し、core の型は StateInfo の中に値で持つ。

MANAKA は USE_CLASSIC_EVAL と USE_PIECE_VALUE を使う。
USE_EVAL_LIST と USE_HUMANLIKE_OPTIONS は使わない。BonaPiece は成駒を金に
まとめ、玉を含まないので Manaka のトークンを復元できないためである。
したがって evaluate.h の BonaPiece 条件には EVAL_MANAKA を足さない。
position.cpp の evaluate_common.h の include 条件も変更しない。

## ビルド

```sh
make -j normal YANEURAOU_EDITION=YANEURAOU_ENGINE_MANAKA TARGET_CPU=AVX2 COMPILER=g++
```

- 次の翻訳単位だけに `-fno-fast-math -ffp-contract=off` が付く (`-ffast-math` の後ろに付き、後ろが勝つ)。
  core/ の全部 (tokens.cpp、q16.cpp と、開発用の MNW1 を入れるときの mnw.cpp、eval_f32.cpp)、
  manaka_net_*.cpp、manaka_output_map.cpp。evaluate_manaka.cpp と他のエディションのフラグは変わらない。
  normal ターゲットは CPPFLAGS をコマンドラインで渡すので、Makefile の指定には override が要る。
  - q16.cpp の dense の後処理 (inv_k * acc + b) は、乗算と加算を FMA に融合すると別の f32 になる。
  - `-ffast-math` は `-ffinite-math-only` を含み、`std::isfinite` が定数 true に畳まれうる。pre の有限性の判定と
    float から int への変換は厳密な翻訳単位 (manaka_output_map.cpp、manaka_net_*.cpp、core) に置く。
    同じコードを `-ffast-math` でビルドすると test manaka_walk の output_map が失敗することを確かめた。
- 既定の `-fno-exceptions` のまま通る。core は例外を使わない。
- `make clean` はエディションを付けないと MANAKA のオブジェクトを消さない。-D の違いは依存ファイルに
  現れないので、フラグを切り替えるときは `make clean YANEURAOU_EDITION=YANEURAOU_ENGINE_MANAKA` を使う。
- 開発用の MNW1 (.mnw、f32) は `EXTRA_CPPFLAGS=-DMANAKA_DEV_MNW1` を付けたときだけ読み込む。付けないビルドは
  「開発用形式」と言って拒否する。公開物ではない。

## Eval の入口

評価の共通宣言は evaluate.h にある。接着側は次を実装する。

- add_options(OptionsMap&, ThreadPool&)
- load_eval()
- compute_eval(const Position&)
- evaluate(const Position&)
- evaluate_with_no_return(const Position&)
- print_eval_stat(Position&)

compute_eval と evaluate は手番側視点の Value を返す。単位は PawnValue=90 の内部単位で、USI の cp そのものではない。
内部値は `trunc(clamp(pre, +-atanh(0.9999)) * 1512.173 * 0.9)` で、最後に一度だけ切り捨てる
(cp を先に切り捨ててから 0.9 を掛けない)。ネットワークの cp (0.9 を掛けない) とは別の量で、
YaneuraOu の FV_SCALE は掛けない。VALUE_MAX_EVAL で頭打ちにする (内部値の最大は 6739 で、実際には届かない)。

add_options は EvalDir (既定 eval)、EvalFile (既定 nn.bin)、ManakaRequireChecksum (既定 false) を登録する。
ファイルは実行ファイルのフォルダ、EvalDir、EvalFile の順で Path::Combine する。
絶対パスならその指定を優先する。どの値も、変わると読み込み済みフラグを倒し、次の isready で読み直す。

### ファイル形式とチェックサム

| ModelType | 末尾の trailer | 読み込み |
|---|---|---|
| SYMq16c | 必須: u32(hash+7) と、それより前の全バイトの CRC-32C (u32) | CRC-32C を検証して読む。不一致は壊れたファイルとして拒否 |
| SYMq16 (旧型) | なし | 読める。ただし内容は検証されない |

- 起動ログに結果を含める: `info string Manaka: SYMq16c (nn.bin, integer) h512, pair16_d128_o3, checksum_verified=verified`。
  旧型は `SYMq16 ... checksum_verified=unverified` に続けて、読み込みごとに一度だけ
  `info string Warning: nn.bin has no checksum (SYMq16); contents are not verified` を出す。警告は接着層が出し、core は出さない。
- ManakaRequireChecksum が真なら、旧型を拒否する (`Error! : failed to read <path> : checksum required but the file is
  ModelType=SYMq16 without a checksum trailer`、終了コード 1、readyok なし)。開発用の MNW1 もチェックサムが無いので拒否する。
- CRC 不一致、trailer の欠落や破損、末尾の余りは、他の破損と同じ扱い: `Error!`、終了コード 1、readyok なし。ゼロ評価で代替しない。
- 検証は Q16Network::load(path, error, require_checksum) と checksum_verified() による。接着層は結果を表示に使うだけ。
- 旧型は、本体の 1 ビットの反転が検出されずに受理される (実測: 本体の中央と最終バイトの反転で、警告つきで readyok)。
  内容を確かめたいときは SYMq16c を使い、ManakaRequireChecksum を真にする。

load_eval は読めなかったら `Error! : failed to read <path> : <理由>` を出して終了する (終了コード 1、readyok は出ない)。
読めないファイルをゼロ評価で代替しない。読み込みの検査は core の Q16Network::load が持つ
(長さ、バージョン、構造文字列と hash、全セクションの hash、全配列の有限性、正の逆尺度、-32768 の不使用、末尾の余り)。

評価できない局面 (SYMq16 はトークンがちょうど 40 個の局面だけ。駒落ちや駒の数が違う局面) は、VALUE_ZERO を返し、
旗を倒したままにして、info string を一度だけ出す。重みの読み込み前に Position::set から compute_eval が
呼ばれたときも VALUE_ZERO を返し、旗には触らない。

## ネットワークの境目 (manaka_net.h)

接着側 (evaluate_manaka.cpp) は、読み込んだ重みの中身を知らない。次の 3 つの型だけを通す。

- Network: 読み込み済みの重み。作ったあとは変わらず、スレッド間で共有する。同じ Network に読み直さない
  (読み直しは新しい Network を作り、世代を進める)。output_map() で内部値への写像を返し、
  new_session() で作業用の状態を作り、describe() で load_eval が出す 1 行を返す。
- Session: 1 スレッドが 1 つ持つ作業用の状態 (約 9 KB の Q16Result を含む)。次の 3 つを持つ。
  pre は手番側から見た tanh の前の f32。失敗したら false で、*pre は変えず、理由を err に入れる。
  - rebuild(board, acc, pre, err): board から acc を一から作り直して評価する。
  - update(board, parent, acc, pre, err): parent (有効で同じ Network が作ったもの) を acc に写し、
    board との差で更新して評価する。parent には書かない。
  - score(board, acc, pre, err): acc がすでに board のトークンの集合のとき、更新せずに評価だけする。
  - rebuild_near(board, acc, pre, err): 親が使えないときの入口。rebuild と同じ値 (整数なのでビット単位で同じ) を
    返すが、Session が自分で持つ直近の局面のアキュムレータのうちトークンの差が小さいものから差分で作る。
    既定の実装は rebuild。force (compute_eval) の全計算は rebuild を使い、直近の局面を使わない。
- load_network(path, require_checksum, err): ファイルの先頭 4 バイトで形式を見分ける。MNW1 の magic は MNW1 (開発用)、
  nn.bin の先頭 (0x7AF32F16) は SYMq16c と SYMq16 の読み込みへ (ModelType の判定は core がする)。
  Network::verification() が kVerified (SYMq16c)、kUnverified (SYMq16)、kNotApplicable (MNW1) を返す。どちらでもなければ拒否する。形式を取り違えて読まず、
  壊れたファイルを別の形式として救済しない。

接着側が持つもの: Position から BoardView への変換 (manaka_board.h)、StateInfo の旗、直前の局面の選び方、
スレッドごとの Session (重みの世代が進んだら作り直す)、OutputMap の適用。
core が持つもの: ファイルの読み込みと検査、トークンの列挙、差分更新、pre の計算。

SYMq16 は cp の定義 (clamp(pre) * 1512.173) を固定で持つので、OutputMap も固定の既定値
(1512.173、0.9、0.9999) を使う。

## アキュムレータと StateInfo

型は YaneuraOu::Eval::Manaka::Accumulator (manaka_accumulator.h)。StateInfo のメンバー名は
manaka_accumulator。64 バイト境界に整列した、trivially copyable な値型で、sizeof は 6464 バイト
(StateInfo 全体は 6976 バイト)。manaka_accumulator は StateInfo の 512 バイト目にあり、
do_move の部分コピー (board_key の手前、56 バイト) の外にある。

| メンバー | 型 | 意味 |
|---|---|---|
| q16 | ManakaCore::Q16Accumulator | 枠ごとの power sum (p1、p2、p3)、pair の和、各枠の 40 トークン (昇順)、Network へのポインタ |
| computed_accumulation | bool | q16 がそろって有効 |
| computed_score | bool | score が現局面の手番側視点の値 |
| score | Value | 内部単位の評価値 |
| weights_gen | uint32_t | 上の値を作ったときの重みの世代。0 は未読み込み |
| owner | uint32_t | q16 を作ったスレッドの番号。0 は誰も作っていない |

core の型を 1 つだけ持つ (別の配置の二重の状態を持たない)。Q16Accumulator は p1、p2、p3 を別の配列に持つので、
丸ごとの memcpy で移さない。祖先から写すときは q16 メンバーを代入する。

**weights_gen。** isready で重みが読み直されると世代が進む。thread.cpp の rootState は
setupStates->back() の丸ごとのコピーなので、旗が真のまま読み直しをまたいで残りうる。
q16.network のポインタだけでは、同じアドレスに作り直された別の重みを見分けられない。
score も accumulation も、weights_gen が現在の世代と違えば使わない (旗が真でも使わない)。

**owner。** Eval::evaluate は Position しか受け取らず、探索スレッドの root の境界 (rootState) を知る手段が無い。
thread.cpp の rootState は setupStates->back() の丸ごとのコピーで、その previous は全スレッドで共有の読み取り専用の
履歴を指す。境界を位置ではなく作成者で表す: rebuild と update で q16 を書いたスレッドが owner に自分の番号 (1 から振り、
再利用しない) を書き、差分更新の親は owner が自分の番号のものだけを使う。rootState のコピーにも共有履歴にも
他のスレッドの印が付いている (丸ごとのコピーは印も写す) ので、親として拒否され、作り直しに回る。
do_null_move は自分の状態のコピーなので自分の印のまま。拒否した回数は foreign_parent に数える。
実エンジンで、position の後に e で評価してから go すると、探索スレッドの rootState のコピーが拒否される (4 スレッド 3 秒で 153 件)。

実際の局面更新との対応は次のとおり。

1. Position::set は StateInfo 全体をゼロにする。両旗は false、weights_gen は 0。
   set が呼ぶ compute_eval は、重みの読み込み済みなら盤から作り直して両旗を立てる。
2. do_move_impl は board_key より前だけをコピーする。manaka_accumulator は後ろなのでコピーされない。
   position.cpp の do_move_impl の冒頭で両旗を必ず false にする。
   兄弟手で同じ StateInfo を再利用しても前の計算結果を使ってはならない。
3. do_null_move は StateInfo 全体をコピーする。トークン集合は手番によらないので q16 は引き継ぎ、
   computed_score だけを false にする。次の評価は score() で、更新せずに評価だけする。
4. undo_move は previous に戻す。接着側で和を逆更新する必要はない。
5. thread.cpp の rootState は、スレッドごとの root の StateInfo。それより前の StateInfo は全スレッドで共有の
   読み取り専用。接着層が書くのは、評価している局面の StateInfo の manaka_accumulator だけで、previous は読むだけ。

pre から内部値への変換は OutputMap::to_internal (manaka_output_map.cpp、厳密な翻訳単位、noinline)。pre が NaN や
無限大なら false を返し、評価は失敗として扱う (両旗を false のまま VALUE_ZERO を返し、info string を一度出す)。
有限なら ±atanh(0.9999) に切ってから掛けるので、int への変換は常に範囲に収まる。

評価 (evaluate_and_keep) の流れ。

1. 重みが未読み込みなら VALUE_ZERO を返す。
2. computed_score が真で weights_gen が現在の世代なら、取ってある score を返す。
3. accumulation が有効で世代が合っていれば (null move の直後)、score() で評価だけする。
4. そうでなければ、accumulation の旗を false にしてから、直前の局面 (st->previous) が有効で、世代が合い、
   owner が自分のものならその q16 から update()、そうでなければ rebuild() する。成功してから
   computed_accumulation を true にし、owner を自分にする。失敗したら両旗を false のままにして VALUE_ZERO を返す。
5. 評価値を確定してから score、weights_gen、computed_score を更新する。

**祖先は直前の局面だけを見る。** 遡って探さず、未計算の祖先を再帰して計算しない。update() は、元の
アキュムレータが有効でさえあれば、どの局面のものからでも正しい (core が自分のトークン一覧と board の差を
取る) ので、直前の局面が本当の親でなくても正しさは変わらない。root 以前の共有履歴は、位置ではなく owner の印で
親にしない (上の owner)。印が自分でない、または無効な親は作り直す。接着層が書くのは評価している局面の
manaka_accumulator だけで、previous には書かない。

**直近の局面からの差分 (rebuild_near)。** 親が使えない (未計算、または owner が自分でない) とき、接着層は
force でなければ Session の rebuild_near を呼ぶ。SYMq16 の Session は、自分が最後に計算した 4 局面のアキュムレータ
(q16 の写し 6.5 KB とトークンのビット集合) を持ち、現局面とトークンの差が最も小さいものを選んで update_from する
(差が 11 を超えるか、持っていなければ全計算。差 d の差分更新は pair を 79d - d^2 行読み、全計算は 780 行)。持っているのは自分のスレッドが rebuild_near と update で作った写しだけで、
StateInfo の previous も他のスレッドの StateInfo も読まないので、root の境界の問題は起きない。結果は選んだ元に依らず
全計算とビット単位で同じ (core は元の token 一覧と現在の board の差を取る)。選び方は速度にだけ効く。重みが読み直されると
Session ごと作り直されるので、古い世代の写しは残らない。実測 (startpos 近辺の bench、depth 12): 作り直しの大半が
差 4 トークン以下の差分になり、nps は base で 1.15 倍。-DMANAKA_VERIFY を付けたビルドは、評価のたびに全計算
(rebuild) とアキュムレータの全要素と pre のビットを突き合わせ、違えば止まる (開発用)。

実測 (4 スレッド、3 秒の探索、startpos): 評価のうち差分更新 44%、作り直し 56%。置換表に評価値があるノードは
静的評価を呼ばないので、そのノードの子は直前の局面が未計算になる。作り直しは 1 回あたり約 5 us 高い
(base、cold 27.0 us と warm 22.1 us。core の高速化前の値)。遡って探す改良は、上の rebuild_near の写しで行う。

## 純 C++17 の core との境目

core/ は C++17 の標準ライブラリだけを使い (例外を使わない)、Position、Piece、Value、
StateInfo、OptionsMap など YaneuraOu の型に依存しない。
接着側が Position から BoardView (core/board.h) を作って core に渡す。
BoardView は cell[81]、hand[2][7]、side_to_move の値型で、**手番は BoardView のメンバー**である。

- 升は Manaka 順、(9-筋)*9+(段-1)。cell[0] は 9a、cell[8] は 9i、cell[72] は 1a、cell[80] は 1i。
  YaneuraOu 順の升番号 y は (8-y/9)*9+y%9 に変換する (core の manaka_square_from_yaneuraou_square)。
- 空は 0。先手駒は kind+1 (1..14)、後手駒は kind+15 (15..28)。
- kind は歩、香、桂、銀、金、角、飛、玉、と、成香、成桂、成銀、馬、龍の順。
- hand の第 1 軸は先手、後手。第 2 軸は歩、香、桂、銀、金、角、飛の順。
- side_to_move は 0 が先手、1 が後手。

Q16Network の入口 (すべて const)。BoardView から: rebuild、update、evaluate。トークンの集合から: make_tokens (BoardView
から 2 枠の昇順の id 一覧と 2344 ビットの集合を作る)、rebuild(Q16Tokens, acc)、update_from(Q16Tokens, base, out)
(out は base からの差分で、base と out は同じでも別でもよい。out の全要素を書く)。pre だけ要るときは
evaluate_pre (Q16Result を作らない)。rebuild_reference、update_reference、evaluate(simd=false) は元のスカラー版で、
速い経路との一致を確かめる基準として残してある。速い経路 (AVX2) は整数の結果を元とビット単位で同じにする。
W1 と W2 の積和は、読み込み時に行をグループに分け、各グループが vpmaddwd の結果を何回まで i32 のまま足してよいかを
行の重みの絶対値の和から証明して決める (入力は絶対値 32767 以下に量子化されるので、全入力に対する証明で、較正集合に
頼らない)。証明できない行は 1 回ごとに i64 へ広げる。

Q16Network は読み込んだあとは const のメソッドだけで評価できるので、スレッド間で共有できる。作業用の領域は
Q16Result で、Session がスレッドごとに持つ。評価の bool の戻り値を必ず確かめる。Q16Result の cp は ネットワークの cp
(0.9 を掛けない) なので、接着側は pre から内部値を作る。

## 検査コマンドと出力

固有の公開宣言は evaluate_manaka.h に置く。共有ファイルは以下の関数を呼ぶ。

| 入口 | 関数 | 引数の文字列 |
|---|---|---|
| test manaka_walk | Eval::Manaka::test_walk(IEngine&, std::istringstream&) | games N plies M seed S |
| test manaka_bench | Eval::Manaka::test_bench(IEngine&, std::istringstream&) | 局面ファイル 回数 [warm\|cold]、または stats [reset] |
| trace_eval | Eval::Manaka::trace(const Position&) | なし |

**trace** は `manaka pre <f32 を %.9g> internal <整数>` を出す。pre は手番側から見た tanh の前の値、internal は
内部値。既存の `eval <整数>` 行はその後に残る。trace_eval は SFEN から新しい Position を作るため、
差分更新の検査には使えない。trace は StateInfo にもアキュムレータにも触らない。

**test manaka_walk** は、固定の乱数で局面を歩き、各局面 (本筋、同じ StateInfo を使い回す兄弟手、null move、
1 から 3 手を評価せずに進めた局面、王手中を含む) で次を確かめる。

1. evaluate の値が、同じ SFEN から作り直した Position の全計算と一致する。
2. 差分で更新した q16 (p1、p2、p3、pair、トークン、Network) が、作り直したものとビット単位で一致する。
3. 先後を入れ替えて 180 度回した局面 (flipped_sfen) の pre が一致する。整数の経路ではビット単位。
   f32 の開発用の経路では足す順が逆になるので、pre の差が 1e-4 以内、内部値の差が 1 以内を合格とする。
4. 重みを読み直したあと、旗を触らずに残しても、取ってある score を返さず、古い世代の親から更新せずに、
   作り直す。値ではなく経路の回数で確かめる (同じファイルを読み直すと値は変わらないため)。
5. 親の作成者 (parent_owner)。有効で世代も合う親でも、owner が他のスレッドのものは差分更新に使わず作り直し、
   自分のものは更新に使う。値は同じなので経路の回数と foreign_parent で確かめる。
6. 出力の写像 (output_map)。NaN、符号付きの無限大、signaling NaN は変換せずに失敗し、*out を変えない。巨大な有限値は
   6739 に頭打ちになり、正負が対称で、0 に向かって切り捨てる。
7. 差分更新と評価だけの経路が 1 度も通らなかったら失敗とする (作り直しだけで合格するのを防ぐ)。

回数は `paths updated N rebuilt N score_only N cached N failed N` で出る。検査のために作り直した Position の
評価は数えない。変異で確かめた (do_move の両旗、do_null_move の score の旗、score と accumulation と親の世代の確認、
親の owner の確認、出力写像の isfinite を外すか fast-math の翻訳単位に置くと、すべて FAILED になる)。

**test manaka_bench** は、評価 1 回の時間を計る。cold は局面ごとに作り直し、warm は局面から 1 手指した子を
親から差分で更新する。平均、中央値、p99、最小、最大 (us) と、経路の回数、zero_values を出す。
局面の読み込みと手を指す時間は含まない。`test manaka_bench stats [reset]` は、これまでに全スレッドが評価した
経路の回数の合計を出す (探索のあとに呼ぶ)。

NNUE 側では LoadAndShare が ModelType=SYM で始まる構造文字列を拒否する。
SYMq16c、SYMq16、不採用の SYM2 を含み、本文を NNUE パラメータとして読み進めない。
拒否時は対象ファイルと理由を info string で出し、`Error! : failed to read nn.bin : FileMissMatch` で終了する。
