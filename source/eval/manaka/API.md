# Manaka 接着 API

U4 の骨組みと U3 の評価実装の境界。土台は develop の f4ead6a4。
以下の行番号は U4 の配線変更後のもの。

## 所有と組み込み

U4 は config.h、Makefile、position.h、position.cpp、
engine/yaneuraou-engine/yaneuraou-search.cpp の trace_eval、
testcmd/normal_test_cmd.cpp のディスパッチ、eval/nnue/evaluate_nnue.cpp の
拒否条件を所有する。共有ファイルは一度だけ変更する。

U4 は最初の骨組みとして evaluate_manaka.h、evaluate_manaka.cpp、
manaka_accumulator.h とこの文書を用意する。骨組みの統合後は、
source/eval/manaka/** 全体を U3 に引き渡す。
core/ 以下は U3a の担当で、U4 は作成も変更もしない。

Makefile の MANAKA ブロックは evaluate_manaka.cpp と通常探索をリンクする。
core のソースを増やす場合は、U3 が共有 Makefile を直接編集するのではなく、
追加する翻訳単位を U4/統合担当に伝える。

MANAKA は USE_CLASSIC_EVAL と USE_PIECE_VALUE を使う。
USE_EVAL_LIST と USE_HUMANLIKE_OPTIONS は使わない。BonaPiece は成駒を金に
まとめ、玉を含まないので Manaka のトークンを復元できないためである。
したがって evaluate.h の BonaPiece 条件には EVAL_MANAKA を足さない。
position.cpp の evaluate_common.h の include 条件も変更しない。

## Eval の入口

評価の共通宣言は evaluate.h にある。接着側は次を実装する。

- add_options(OptionsMap&, ThreadPool&)
- load_eval()
- compute_eval(const Position&)
- evaluate(const Position&)
- evaluate_with_no_return(const Position&)
- print_eval_stat(Position&)

compute_eval と evaluate は手番側視点の Value を返す。
単位は PawnValue=90 の内部単位で、USI の cp そのものではない。
骨組みでは常に VALUE_ZERO を返し、アキュムレータを有効にしない。
重みの読み込み前に Position::set から compute_eval が呼ばれても同じ扱いにする。

add_options は EvalDir（既定 eval）と EvalFile（既定 nn.bin）を登録する。
ファイルは実行ファイルのフォルダ、EvalDir、EvalFile の順で Path::Combine する。
絶対パスならその指定を優先する。値が変わると読み込み済みフラグを倒し、
次の isready で再確認する。骨組みの load_eval はファイルを開けるかを
info string で報告するだけで、重みを読み込まない。ファイルがなくても終了しない。
本実装では形式検査を追加し、不正なファイルをゼロ評価で代替しないこと。

## アキュムレータと StateInfo

型は YaneuraOu::Eval::Manaka::Accumulator。
定義は manaka_accumulator.h、StateInfo のメンバー名は
manaka_accumulator（position.h:219）。64 バイト境界に整列した値型である。

| メンバー | 型と形 | 意味 |
|---|---|---|
| p | int64_t[2][3][128] | 枠ごとの power sum |
| pair | int32_t[2][16] | 枠ごとの pair の和 |
| tok | uint16_t[2][40] | 各枠で先頭 ntok 個が有効なトークン一覧 |
| ntok | uint8_t | 両枠共通のトークン数 |
| computed_accumulation | bool | 和とトークン一覧がそろって有効 |
| computed_score | bool | score が現局面の手番側視点の値 |
| score | Value | 内部単位の評価値 |

配列上限は MANAKA_MAX_DIM、MANAKA_MAX_ORDER、MANAKA_MAX_PAIR_DIM。
既定は 128、3、16。読み手はこれを超える形を拒否する。
可変長の所有物やデストラクタを必要とするオブジェクトは入れない。
骨組みでは配列の内容を更新せず、未計算の配列を読まない。

実際の局面更新との対応は次のとおり。

1. Position::set は position.cpp:432 で StateInfo 全体をゼロにする。
   両旗は false。position.cpp:735 の compute_eval は読み込み前なら
   0 を返して両旗を false のままにする。
2. do_move_impl は position.cpp:1697 で board_key より前だけをコピーする。
   manaka_accumulator は後ろなのでコピーされない。
   position.cpp:1740-1741 で両旗を必ず false にする。
   兄弟手で同じ StateInfo を再利用しても前の計算結果を使ってはならない。
3. do_null_move は position.cpp:2440 で StateInfo 全体をコピーする。
   トークン集合は手番によらないので和と一覧は引き継ぎ、
   position.cpp:2476 で computed_score だけを false にする。
4. undo_move は previous に戻す。接着側で和を逆更新する必要はない。
   thread.cpp:600-601 の rootState はスレッド専用だが、それ以前は共有の
   読み取り専用状態である。本実装は共有祖先を変更しないこと。

本実装での更新順序は次に固定する。

1. 接着側が盤と持ち駒から両枠のトークン一覧を作る。
2. 最大 8 手を初期上限として previous をたどり、最も近い計算済み祖先を探す。
   スレッド専用 root より前には進まない。root 境界の取得方法は U3 が
   スレッドとの接続を確認して決め、共有状態へ書き込まない。
3. 祖先と現局面の一覧から、除去 R、追加 A、共通 K を求める。
   祖先がない場合と、差分の pair 行数が全計算以上の場合は全計算する。
4. 祖先の和を引き継ぎ、power sum は先に R を引き、次に A を足す。
   pair は A-K と A-A の対を加え、R-K と R-R の対を引く。
5. 和とトークン一覧をすべて更新してから computed_accumulation を true、
   computed_score を false にする。評価値を確定してから score と
   computed_score を更新する。

この手順は U3 が実装する。骨組みは do_move と do_null_move で旗を倒すだけである。

## 純 C++17 の core との境目

core/ は C++17 の標準ライブラリだけを使い、Position、Piece、Value、
StateInfo、OptionsMap など YaneuraOu の型に依存しない。
接着側が Position から BoardView を作って core に渡す。
BoardView は 81 マスの駒コードと手駒 2x7 の値型とする。

駒コード、升と手駒の並びは次に固定する。core の宣言ファイルと
メンバーの綴りは U3a の実装に合わせるが、この意味は変えない。

- 升は Manaka 順、(9-筋)*9+(段-1)。YaneuraOu 順の升番号 y は
  (8-y/9)*9+y%9 に変換する。
- 空は 0。先手駒は kind+1（1..14）、後手駒は kind+15（15..28）。
- kind は歩、香、桂、銀、金、角、飛、玉、と、成香、成桂、成銀、馬、龍の順。
- 手駒の第 1 軸は先手、後手。第 2 軸は歩、香、桂、銀、金、角、飛の順。
- 手番は BoardView とは別の 0（先手）または 1（後手）で渡す。

core が持つアキュムレータの演算型と StateInfo 内の型が異なる場合、
接着側が明示的に変換する。core に YaneuraOu の Value を含ませない。

## 検査コマンドと出力

固有の公開宣言は evaluate_manaka.h に置く。
共有ファイルは以下の関数を呼ぶので、U3 は名前と引数を維持する。

| 入口 | 関数 | 引数の文字列 |
|---|---|---|
| test manaka_walk | Eval::Manaka::test_walk(IEngine&, std::istringstream&) | games N plies M seed S |
| test manaka_bench | Eval::Manaka::test_bench(IEngine&, std::istringstream&) | 局面ファイル 回数 [warm\|cold] |
| trace_eval | Eval::Manaka::trace(const Position&) | なし |

骨組みの test 関数は未実装と sync_cout で報告するだけである。
trace は `manaka pre 0 internal 0` を出す。
本実装の行形式は `manaka pre <f32 を %.9g> internal <整数>`。
既存の `eval <整数>` 行はその後に残す。
trace_eval は SFEN から新しい Position を作るため、差分更新の検査には使えない。

NNUE 側では LoadAndShare が ModelType=SYM で始まる構造文字列を拒否する。
旧 SYM、SYMf32、SYM2 を含み、本文を NNUE パラメータとして読み進めない。
拒否時は対象ファイル、ModelType、対応しない理由を info string で出す。
