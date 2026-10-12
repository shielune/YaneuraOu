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
