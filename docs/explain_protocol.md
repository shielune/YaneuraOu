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
| `parts` | 先手視点の内訳 `material` `tempo` `speed` `king` `activity`。モデルを読んでいるときのみ（合計が評価値） |
| `sfen` | その局面の SFEN |

`legacy` 付きは、これに加えて旧線形内訳を出す: `black_pov` / `material` / `mobility` /
`king` / `tempo` と、2 局面目以降は `d_material` / `d_mobility` / `d_king` / `d_tempo` と
`top`（直前から寄与が大きく動いた列を `top <n>` 個、既定 5。`top 0` で空配列）。
`top` の `name` は ASCII の識別子（例 `mobility.bishop(2,-2)@rank7`、
持ち駒を打つ利きは `mobility.drop.pawn(0,1)`、`king.ring1.enemy_attacks_by.horse`）。
駒名は pawn / lance / knight / silver / gold / bishop / rook / king / horse / dragon /
promoted_pawn / promoted_lance / promoted_knight / promoted_silver。

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
{"pov":"black","phase":0,"parts":["material","tempo","speed","king","activity"]}
{"ply":0,"moves_left":89,"parts":{"material":0,"tempo":10,"speed":0,"king":0,"activity":0},"sfen":"lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1"}
{"ply":1,"move":"7g7f","see":0,"gives_check":false,"moves_left":88,"parts":{"material":0,"tempo":-10,"speed":6,"king":-21,"activity":43},"sfen":"lnsgkgsnl/1r5b1/ppppppppp/9/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL w - 2"}
{"ply":2,"move":"3c3d","see":0,"gives_check":false,"moves_left":87,"parts":{"material":0,"tempo":11,"speed":6,"king":0,"activity":0},"sfen":"lnsgkgsnl/1r5b1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL b - 3"}
{"ply":3,"move":"8h2b+","see":0,"gives_check":false,"moves_left":78,"parts":{"material":959,"tempo":-18,"speed":-2,"king":-394,"activity":-123},"sfen":"lnsgkgsnl/1r5+B1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/7R1/LNSGKGSNL w B 4"}
{"ply":4,"move":"3a2b","see":945,"gives_check":false,"moves_left":79,"parts":{"material":0,"tempo":18,"speed":21,"king":-40,"activity":28},"sfen":"lnsgkg1nl/1r5s1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/7R1/LNSGKGSNL b Bb 5"}
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
2. ヘッダ行 → 局面行（候補順、各候補は ply 昇順） → `{"done":true,...}`
3. `bestmove ...`（完了通知。これより後に `explain` の出力は出ない）

`stop`、`infinite`、`ponder` でも同じ。探索スレッドがすべて止まった後、`bestmove` を出す前に
探索を終える側のスレッドが同期的に出力する。USI の主スレッドが持つ局面は使わず、
探索開始局面の SFEN から作った専用の局面で進める。定跡や宣言勝ちで探索せずに終わる場合は
その指し手（と ponder 手）だけを読み筋として扱い、合法手が無ければ `root` を含め読み筋が無いので、`root` 行だけ（または空）の後に `done` と `bestmove resign` が出る。

### ヘッダ行と局面行

ヘッダ行は `pov` `phase` `parts` に加え、`root_sfen`（探索開始局面）と `multipv`（読み筋が空でない候補の数）。

局面行は `kind` で区別する 3 種だけ。読み筋の途中の局面は出さない（制限ではなく仕様）。
空の読み筋の候補は出さない。`done` の `positions` は出した局面行の数。

| kind | 出る回数 | 内容 | キー |
| --- | --- | --- | --- |
| `root` | 全体で 1 回 | 現局面。`parts` は**素の局面**（静止探索なし）、`qsearch_parts` は**静止探索後**の値 | `sfen` `moves_left` `parts` `qsearch_sfen` `qsearch_moves` `qsearch_parts` |
| `after_move` | 候補ごとに 1 回 | 候補の 1 手目を指した直後。**静止探索しない**素の局面 | `multipv` `ply`(=1) `move` `see` `gives_check` `sfen` `moves_left` `parts` |
| `leaf` | 候補ごとに 1 回 | 読み筋の最終局面。**静止探索の後**の値 | `multipv` `ply`(=読み筋の長さ) `move`(=最終手) `sfen` `qsearch_sfen` `qsearch_moves` `moves_left` `parts` |

読み筋が 1 手のときは `after_move` と `leaf` が同じ局面を指すが、前者は素、後者は静止探索後で意味が違うので両方出す。
`multipv` と `ply` と `kind` で行を特定できる。`root` に `multipv` と `ply` は無い。
`qsearch_moves` はすでに静かなら空で、そのとき `qsearch_sfen` は `sfen` と同じ（`root` では `qsearch_parts` も `parts` と同じ）。

`root` の `parts` を素にしているのは、取り合いの途中の局面で「今の駒損」を見せるため。
`qsearch_parts` と並べると、取り合いの後でどう変わるかが読める。

#### 静止探索の扱いと `after_move` の意味

モデルは「静止探索後の評価値」に合わせて学習されている。`leaf` はこの条件に合わせ、
`sfen` から既存の `qsearch_pv`（`qsearch_psv` と同じ実装）で静かな局面まで進めた
`qsearch_sfen` で `parts` を計算する。

`root.parts` と `after_move` の `parts` は、**静止探索後の局面で学習されたモデルを、素の局面にそのまま当てた参考値**で、
取り合いの途中では大きく偏る。それが目的で、角を切るような一時的な駒損得を見せるために、あえて静止探索しない。
読み筋の先（`leaf`）で回復していれば、`after_move` の駒損が一時的なものだと読める。

`qsearch` はトランスポジションテーブルを使うが、`nodes` などの探索の統計は元に戻す。
`legacy` / `top` は `go explain` では使えない。`see` `gives_check` は `after_move` のみ（指す前の局面での値）。

### 例

`position startpos moves 7g7f 3c3d` に `go explain depth 8 searchmoves 8h2b+`（MultiPV 1）。
`root` は初期配置に近い静かな局面で、`qsearch_moves` は空、`qsearch_parts` は `parts` と同じ。
`8h2b+` 直後（`after_move`）は角を取った直後なので `material` が約 +959 と大きくプラスに出る。
読み筋の最終局面（`leaf`）では取り返されて `material` は 0 に戻る。

```json
{"pov":"black","phase":0,"parts":["material","tempo","speed","king","activity"],"root_sfen":"lnsgkgsnl/1r5b1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL b - 3","multipv":1}
{"kind":"root","moves_left":87,"parts":{"material":0,"tempo":11,"speed":6,"king":0,"activity":0},"qsearch_parts":{"material":0,"tempo":11,"speed":6,"king":0,"activity":0},"sfen":"lnsgkgsnl/1r5b1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL b - 3","qsearch_sfen":"lnsgkgsnl/1r5b1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL b - 3","qsearch_moves":[]}
{"kind":"after_move","multipv":1,"ply":1,"move":"8h2b+","see":0,"gives_check":false,"moves_left":78,"parts":{"material":959,"tempo":-18,"speed":-2,"king":-394,"activity":-123},"sfen":"lnsgkgsnl/1r5+B1/pppppp1pp/6p2/9/2P6/PP1PPPPPP/7R1/LNSGKGSNL w B 4"}
{"kind":"leaf","multipv":1,"ply":9,"move":"4i5h","moves_left":83,"parts":{"material":0,"tempo":-14,"speed":-6,"king":-4,"activity":-62},"sfen":"lnsgkg1nl/1r7/p1ppppspp/6p2/1p7/2P4P1/PPSPPPP1P/4G2R1/LN1GK1SNL w Bb 12","qsearch_sfen":"lnsgkg1nl/1r7/p1ppppspp/6p2/1p7/2P4P1/PPSPPPP1P/4G2R1/LN1GK1SNL w Bb 12","qsearch_moves":[]}
{"done":true,"positions":3}
```

取り合いの途中の局面の `root` の例（`position sfen lnsgk1snl/6g2/p1pppp2p/6R2/9/1rP6/P2PPPP1P/1+bG6/LNS1KGSNL b 3Pb2p 17`）。
先手の角が 2 枚とも相手の手にあり、素の `parts.material` は -877 と大きな駒損に出るが、
`7i8h` で馬を取り返せるので、静止探索後の `qsearch_parts.material` は 75 になる。

```json
{"kind":"root","moves_left":51,"parts":{"material":-877,"tempo":52,"speed":11,"king":233,"activity":74},"qsearch_parts":{"material":75,"tempo":-50,"speed":20,"king":-66,"activity":-5},"sfen":"lnsgk1snl/6g2/p1pppp2p/6R2/9/1rP6/P2PPPP1P/1+bG6/LNS1KGSNL b 3Pb2p 17","qsearch_sfen":"lnsgk1snl/6g2/p1pppp2p/6R2/9/1rP6/P2PPPP1P/1SG6/LN2KGSNL w B3Pb2p 18","qsearch_moves":["7i8h"]}
```
