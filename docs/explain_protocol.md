# explain の出力プロトコル

```text
explain [legacy] [top <n>] [moves <move1> <move2> ...]
```

`legacy` と `top` は `moves` より前に置く。順序は `legacy`、`top` のどちらが先でもよい。
`moves` を省くと現局面だけを出す。局面は変わらない（指した手はすべて戻す）。
出力は常に ASCII で、1 行 1 JSON オブジェクト。

## 行の種類

出力は次の順に並ぶ。

1. ヘッダ行（1 行）
2. 局面行（`ply` 0 が根、以降は 1 手ごとに 1 行）
3. 不正手があればエラー行（0 または 1 行）
4. 終端行 `{"done":true,...}`（必ず 1 行）

読み手は `done` 行を見るまで読み続ければよい。`explain` に付随する
`info string`（`HceExplainFile: loaded ...` など）は option 設定時のもので、
`explain` 実行中に不正手で `info string Error!` が出ることはない。

### ヘッダ行

非 legacy は `pov`（評価値の視点。常に `black`）、`phase`（根の進行度 0〜1）、
`parts`（局面行の `parts` に入る項の名前）の三つだけ。

`legacy` 付きは従来のヘッダ（`route` / `weights` / `variant` / `pieces` / `features` /
`phase` / `pov` / `caveat`）。`caveat` の日本語は `\uXXXX` でエスケープされる。

### 局面行

| キー | 内容 |
| --- | --- |
| `ply` | 0 が根。以降は読み筋の何手目か |
| `move` | その局面に至る手（USI）。根には無い |
| `see` | 指す前の局面での静的交換値。根には無い |
| `gives_check` | その手が王手か。根には無い |
| `moves_left` | 進行度モデルによる残り手数。`HceProgressFile` を読んでいるときのみ |
| `parts` | 先手視点の内訳 `material` `tempo` `threat` `king_safety` `mobility`。モデルを読んでいるときのみ（合計が評価値） |
| `sfen` | その局面の SFEN |

`legacy` 付きは、これに加えて旧線形内訳を出す: `black_pov` / `linear_material` / `linear_mobility` /
`linear_king` / `linear_tempo` と、2 局面目以降は `d_linear_material` / `d_linear_mobility` / `d_linear_king` / `d_linear_tempo` と
`top`（直前から寄与が大きく動いた列を `top <n>` 個、既定 5。`top 0` で空配列）。
`top` の `name` は ASCII の識別子（例 `mobility.bishop(2,-2)@rank7`、
持ち駒を打つ利きは `mobility.drop.pawn(0,1)`、`king.ring1.enemy_attacks_by.horse`）。
駒名は pawn / lance / knight / silver / gold / bishop / rook / king / horse / dragon /
promoted_pawn / promoted_lance / promoted_knight / promoted_silver。

#### parts の 5 項目

| キー | 意味 |
| --- | --- |
| `material` | 駒の価値の合計 |
| `tempo` | 手番を持つことの価値（符号は手番で反転する） |
| `threat` | 王手と短い詰み（1 手詰め・3 手詰め）から見た、どちらが先に詰ませそうか |
| `king_safety` | 玉の安全度 |
| `mobility` | 利き（各駒の利きの量と質）を入力にしたネットワーク |

オプション `HceThreat` を真にすると、局面行に `"threat":{...}`（`threat` の入力になる 8 個の数。
`mover.checks` など）も出る。局面ごとに詰みを探すので既定は偽。`parts.threat` とは別のキー。
内部の名前（モデルファイルの節名）は旧名のままで、出力キーだけが上の名前になっている。

### 手番を渡した局面（`pass`）と詰めろ（`mate_threat`）

どちらもオプションで有効にしたときだけ出る。既定は偽で、偽のときの出力は今までと一字一句同じ
（探索の nodes も変わらない）。legacy 形式には出ない。

| オプション | 既定 | 出るキー |
| --- | --- | --- |
| `HcePass` | false | `pass` |
| `HceMateThreat` | false | `mate_threat` |
| `HceMateThreatNodes` | 1000000 | `mate_threat` の DfPn の探索量の上限（ノード数、1 局面あたり） |

どちらも、その行の `sfen`（`leaf` は `qsearch_sfen`）の盤面で、手番だけを相手に渡した局面について答える。
root の行は、素の局面の組を `pass` / `mate_threat`、静止探索後の局面の組を
`qsearch_pass` / `qsearch_mate_threat` に出す（`parts` / `qsearch_parts` と同じ対応）。
`explain moves` の各 ply と `pv` / `leaf` の行は `pass` / `mate_threat` のみ。

- `pass`: `{"moves_left":N,"parts":{...}}`。`parts` と同じ 5 項目（先手視点）を、手番を渡した局面で
  計算したもの。モデルを読んでいないときは出ない。手番側に王手がかかっていて手番を渡せないときは `null`。
- `mate_threat`: `{"moves":["4h7h","7i7h",...],"nodes":N}` または `null`。手番を渡したとき、
  もとの手番でない側に詰みがある（詰めろ）なら、DfPn が証明した詰みの手順（`moves` の先頭が、
  手番を渡された側が指す手）と使ったノード数。詰みを証明できなかったとき（不詰、上限、メモリ不足）と
  王手がかかっているときは `null`。詰めありと答えたときは証明済みだが、`null` は詰めろが無い保証ではない
  （上限で見逃すことがある）。

`pass.parts.threat`・`parts.threat` は王手と短い詰みから評価に入れた「速さ」の項で、`mate_threat` は
DfPn で詰みを証明できたかどうかの別の出力。`HceThreat` は `threat` の入力 8 個の数を出すオプションで、
`HceMateThreat` とは無関係。

### エラー行と終端行

不正手（その局面で指せない手、解釈できない文字列）に当たると、そこまでの有効な局面を
出したうえで次の 1 行を出して打ち切る。

```json
{"ply":2,"move":"7g7f","error":"illegal move"}
```

`ply` は不正手の位置（有効な手の数 + 1）、`move` は渡された文字列。

終端行の `positions` は出力した局面行の数（根を含む）。エラーで打ち切ったときは
`"error":true` が付く。重みが読めないときの `{"error":"no weights loaded"}` の後にも出る。

```json
{"done":true,"positions":5}
{"done":true,"positions":2,"error":true}
```

## 全体例（非 legacy）

```text
position startpos
explain moves 7g7f 3c3d 8h2b+ 3a2b
```

```json
{"pov":"black","phase":0,"parts":["material","tempo","threat","king_safety","mobility"]}
{"ply":0,"moves_left":89,"parts":{"material":0,"tempo":10,"threat":0,"king_safety":0,"mobility":0},"sfen":"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1"}
{"ply":1,"move":"7g7f","see":0,"gives_check":false,"moves_left":88,"parts":{"material":0,"tempo":-10,"threat":6,"king_safety":-21,"mobility":43},"sfen":"lnsgkgsnl/1r5b1/ppppppppp/9/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL w - 2"}
{"ply":2,"move":"3c3d","see":0,"gives_check":false,"moves_left":87,"parts":{"material":0,"tempo":11,"threat":6,"king_safety":0,"mobility":0},"sfen":"lnsgkgsnl/1r5b1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL b - 3"}
{"ply":3,"move":"8h2b+","see":0,"gives_check":false,"moves_left":78,"parts":{"material":959,"tempo":-18,"threat":-2,"king_safety":-394,"mobility":-123},"sfen":"lnsgkgsnl/1r5+B1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/7R1/LNSGKGSNL w B 4"}
{"ply":4,"move":"3a2b","see":945,"gives_check":false,"moves_left":79,"parts":{"material":0,"tempo":18,"threat":21,"king_safety":-40,"mobility":28},"sfen":"lnsgkg1nl/1r5s1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/7R1/LNSGKGSNL b Bb 5"}
{"done":true,"positions":5}
```

上は `HceProgressFile` と `HceExplainFile` を読んだ状態の実出力（値は同梱モデルのもの）。

## go explain

```text
go explain <通常の go 指定>      例: go explain movetime 300 / go explain depth 18 / go explain infinite
```

`explain` は `go` の他の指定（`depth` `nodes` `movetime` `byoyomi` `infinite` `ponder` など）と
併用できる。`explain` を付けない `go` の動作と出力は変わらない。`explain` は HCE_EXPLAIN 版のみ。

探索が終わると、`MultiPV` の全候補の読み筋について、根から末尾まで全局面を explain する。
手数や候補数の制限はない（explain は静的評価なので安い）。

### 出力の順序

1. 通常の `info ...`（最終 PV を含む）
2. ヘッダ行 → `root` → 候補ごとに（`pv` を ply 昇順、最後に `leaf`） → `{"done":true,...}`
3. `bestmove ...`（完了通知。これより後に `explain` の出力は出ない）

`stop`、`infinite`、`ponder` でも同じ。探索スレッドがすべて止まった後、`bestmove` を出す前に
探索を終える側のスレッドが同期的に出力する。USI の主スレッドが持つ局面は使わず、
探索開始局面の SFEN から作った専用の局面で進める。定跡や宣言勝ちで探索せずに終わる場合は
その指し手（と ponder 手）だけを読み筋として扱い、合法手が無ければ `root` を含め読み筋が無いので、`root` 行だけ（または空）の後に `done` と `bestmove resign` が出る。

定跡に当たって探索をスキップした場合も、定跡が `info ... multipv i` で列挙した全候補を
`multipv` 1..N として出す（`MultiPV` の値では切らない。通常の `go` が全候補を出すのに揃える）。
各候補の読み筋は定跡が `info` に出した PV（指し手と定跡の先読み。`BookPvMoves` で決まる手数）と同じで、
ヘッダの `multipv` は候補数、`leaf` の本数と一致する。選ばれた手の最後の `info depth 0 multipv 1` は
候補の重複なので数えない。

### ヘッダ行と局面行

ヘッダ行は `pov` `phase` `parts` に加え、`root_sfen`（探索開始局面）と `multipv`（読み筋が空でない候補の数）。
空の読み筋の候補は出さない。`done` の `positions` は出した局面行の数。

局面行は `kind` で区別する 3 種。読み筋の各局面は静的評価（静止探索なし）で 1 手ずつ全手出し、
その末端局面を静止探索した後の局面（`leaf`）の値を候補ごとに 1 行出す。
**`leaf` は読み筋の末端局面を静止探索した後の局面**で、`sfen` が読み筋の最終局面、`qsearch_sfen` が葉。静止探索の途中の局面は出さず、手順を `qsearch_moves` に入れる。

| kind | 出る回数 | 内容 | キー |
| --- | --- | --- | --- |
| `root` | 全体で 1 回 | 現局面（ply 0）。`parts` は素の局面、`qsearch_parts` は root からの静止探索後の値 | `sfen` `moves_left` `parts` `qsearch_sfen` `qsearch_moves` `qsearch_parts` |
| `pv` | 候補の読み筋の手数ぶん | 読み筋の ply 手目を指した直後の局面。**静止探索しない**素の局面。手数の制限はない | `multipv` `ply`(1..読み筋の長さ) `move` `see` `gives_check` `sfen` `moves_left` `parts` |
| `leaf` | 候補ごとに 1 回（`pv` の最後の次） | 読み筋の末端局面を静止探索した後の局面（葉）の値 | `multipv` `ply`(=読み筋の長さ) `sfen`(読み筋の最終局面) `qsearch_sfen` `qsearch_moves` `moves_left` `parts`(葉の値) |

`multipv` と `ply` と `kind` で行を特定できる。`root` に `multipv` と `ply` は無い。
`qsearch_moves` が空なら静かな局面で、`qsearch_sfen` は `sfen` と同じ。`root` では `qsearch_parts` が
`parts` と同じ、`leaf` では `parts` が末尾の `pv` 行と同じになる。

#### 静止探索の扱い

モデルは「静止探索後の評価値」に合わせて学習されている。`pv` 行と `root.parts` は静止探索をかけない
**素の局面にモデルをそのまま当てた参考値**で、取り合いの途中なら大きく偏る。それが目的で、
角を切るような一時的な駒損得をそのまま見せる。取り返した後の値は、`root.qsearch_parts` と
`leaf` 行（モデルの学習条件に合う値）で読む。

静止探索は既存の `qsearch_pv`（`qsearch_psv` と同じ実装）を使う。トランスポジションテーブルを使うため、
同じ局面でも探索の状態で `qsearch_moves` が変わることがある。`nodes` などの探索の統計は元に戻す。
`legacy` / `top` は `go explain` では使えない。`see` `gives_check` は `pv` 行のみ（指す前の局面での値）。

### 例 1: 角を切って取り返す読み筋

`position sfen lnsgk1snl/6g2/p1pppp2p/2r3R2/9/9/P1SPPPP1P/2G6/LN2KGSNL b B3Pb3p 21` に
`go explain movetime 1000`（MultiPV 3）。最善手 `3d7d` は飛で角を取る手で、`ply` 1 の
`material` は +941 と大きくプラスに出る。`ply` 2 の `7c7d`（取り返し）で 0 に戻る。
`qsearch_moves` が空の `leaf` 行の `parts` は、末尾の `pv` 行と同じ。
下は multipv 1 の全行（multipv 2、3 は同じ形でそれぞれ 12 手、17 手続く）。

```json
{"pov":"black","phase":0.195122,"parts":["material","tempo","threat","king_safety","mobility"],"root_sfen":"lnsgk1snl/6g2/p1pppp2p/2r3R2/9/9/P1SPPPP1P/2G6/LN2KGSNL b B3Pb3p 21","multipv":3}
{"kind":"root","moves_left":46,"parts":{"material":0,"tempo":53,"threat":66,"king_safety":-19,"mobility":27},"qsearch_parts":{"material":0,"tempo":65,"threat":123,"king_safety":-12,"mobility":11},"sfen":"lnsgk1snl/6g2/p1pppp2p/2r3R2/9/9/P1SPPPP1P/2G6/LN2KGSNL b B3Pb3p 21","qsearch_sfen":"lnsgk1snl/6g2/p2ppp2p/2p6/9/9/P1SPPPP1P/2G6/LN2KGSNL b RB3Prb3p 23","qsearch_moves":["3d7d","7c7d"]}
{"kind":"pv","multipv":1,"ply":1,"move":"3d7d","see":0,"gives_check":false,"moves_left":43,"parts":{"material":941,"tempo":-77,"threat":-6,"king_safety":-89,"mobility":-55},"sfen":"lnsgk1snl/6g2/p1pppp2p/2R6/9/9/P1SPPPP1P/2G6/LN2KGSNL w RB3Pb3p 22"}
{"kind":"pv","multipv":1,"ply":2,"move":"7c7d","see":990,"gives_check":false,"moves_left":46,"parts":{"material":0,"tempo":65,"threat":123,"king_safety":-12,"mobility":11},"sfen":"lnsgk1snl/6g2/p2ppp2p/2p6/9/9/P1SPPPP1P/2G6/LN2KGSNL b RB3Prb3p 23"}
{"kind":"leaf","multipv":1,"ply":2,"moves_left":46,"parts":{"material":0,"tempo":65,"threat":123,"king_safety":-12,"mobility":11},"sfen":"lnsgk1snl/6g2/p2ppp2p/2p6/9/9/P1SPPPP1P/2G6/LN2KGSNL b RB3Prb3p 23","qsearch_sfen":"lnsgk1snl/6g2/p2ppp2p/2p6/9/9/P1SPPPP1P/2G6/LN2KGSNL b RB3Prb3p 23","qsearch_moves":[]}
```

### 例 2: 取り合いの途中の root

`position sfen lnsgk1snl/6g2/p1pppp2p/6R2/9/1rP6/P2PPPP1P/1+bG6/LNS1KGSNL b 3Pb2p 17`。
先手の角が 2 枚とも相手の手にあり、素の `parts.material` は -877 と大きな駒損に出る。
`7i8h` で馬を取り返せるので、静止探索後の `qsearch_parts.material` は 75 になる。

```json
{"kind":"root","moves_left":51,"parts":{"material":-877,"tempo":52,"threat":11,"king_safety":233,"mobility":74},"qsearch_parts":{"material":75,"tempo":-50,"threat":20,"king_safety":-66,"mobility":-5},"sfen":"lnsgk1snl/6g2/p1pppp2p/6R2/9/1rP6/P2PPPP1P/1+bG6/LNS1KGSNL b 3Pb2p 17","qsearch_sfen":"lnsgk1snl/6g2/p1pppp2p/6R2/9/1rP6/P2PPPP1P/1SG6/LN2KGSNL w B3Pb2p 18","qsearch_moves":["7i8h"]}
```
