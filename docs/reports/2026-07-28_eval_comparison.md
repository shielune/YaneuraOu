# 評価関数ごとの評価値比較 (depth 16)

計測日: 2026-07-28 / ブランチ: `feature/upstream-v9.6x`

同一局面を depth 16 まで読ませて、各評価関数がどんな評価値を返すかを並べたもの。
**upstream V9.6x のエンジンで手持ちの評価関数が全て読めるか**の確認も兼ねている。

## 計測条件

| | |
|---|---|
| 局面 | `l1g1k2nl/1r4g2/2nsppsp1/p1pp2p1p/1p4PP1/P1P2P2P/1PSPPS3/2GK1G1R1/LN5NL w Bb 32` |
| 探索 | `go depth 16` |
| ビルド | native (clang-14 / `TARGET_CPU=OTHER` スカラー) と WASM (em++ 6.0.4 / node / pthread) |
| 固定 | `Threads=1` / `USI_Hash 256` / `usinewgame` / 定跡オフ / `PvInterval 0` |
| `FV_SCALE` | **エンジン既定の 16 のまま** |

後手番 (`w`) の局面なので、**プラスが後手優勢**。

ビルドにスカラーを選んだのは、NEON ビルドが評価値で乖離するため
([`../wasm_native_divergence.md`](../wasm_native_divergence.md))。
スカラーと WASM SIMD は浅い深さでビット一致するので、こちらを基準とする。

⚠ `FV_SCALE` は評価値のスケールを直接変える。配布元が推奨値を指定している
評価関数 (NAGISA_V3 は 28 など) では、実運用の数値はこの表と変わる。
今回は評価関数ごとの推奨値を持っていないため、全て既定値で揃えた。

## 結果 (Threads=1 / `USI_Hash 256` / 決定的)

`score` は後手番の局面なので**プラスが後手優勢**。

| 評価関数 | アーキテクチャ | サイズ | native | WASM | bestmove |
|---|---|---|---|---|---|
| `k_p_256/uonuma` | KP256 | 0.9 MB | **cp 56** | **cp 54** | `3d3e ponder 2i3g` |
| `k_p_256/suisho` (水匠 petite) | KP256 | 0.9 MB | **cp 52** | **cp 51** | `3d3e ponder 2i3g` |
| `halfkp_768/aoba` (AobaNNUE) | HalfKP 768x2-16-64 | 183.7 MB | **cp 28** | **cp 28** | `3d3e ponder 2i3g` |
| `halfkp_256/suisho5` (水匠5) | HalfKP 256x2-32-32 | 61.2 MB | **cp 8** | **cp 94** | `3d3e ponder 2i3g` |
| `halfkp_256/hao` | HalfKP 256x2-32-32 | 61.2 MB | **cp -43** | **cp -46** | `3d3e ponder 2i3g` |
| `halfka_hm2_1024/nagisa` (NAGISA_V3) | SFNN HalfKA_hm2 1024x2-15-64 k3k3 (LayerStack 9) | 74.8 MB | **cp -76** | **cp -54** | `3d3e ponder 2i3g` |

探索ノード数:

| 評価関数 | native | WASM |
|---|---|---|
| `k_p_256/uonuma` | 294,224 | 215,106 |
| `k_p_256/suisho` | 309,313 | 269,297 |
| `halfkp_768/aoba` | 582,688 | 582,688 |
| `halfkp_256/suisho5` | 368,195 | 743,902 |
| `halfkp_256/hao` | 214,377 | 241,948 |
| `halfka_hm2_1024/nagisa` | 282,494 | 389,416 |

### 読み取れること

- **12 通り (6評価関数 × native/WASM) すべてが同じ最善手 `3d3e` (ponder `2i3g`) を返した**
- ⚠ ただし `halfka_hm2_1024/nagisa` は **bucket 選択が学習時と食い違っており、
  本来の強さで測れていない** (下記「NAGISA_V3 の bucket 不一致」)。
  読み込めて、それらしい値を返すが、別物として扱うこと
- 評価値は cp -76 〜 +56 と 130cp の幅があるが、これは評価関数ごとの
  **スケールの違い**を多分に含む。`FV_SCALE` を揃えていない以上、
  この表から「どれが強い」は読み取れない
- **`halfkp_768/aoba` だけ native と WASM がノード数まで完全一致**した (582,688)。
  他は depth 16 に届くまでの探索木がずれる。これは既知の
  [native/WASM 乖離](../wasm_native_divergence.md) で、局面と評価関数によって
  出たり出なかったりする
- 差が大きいのは `halfkp_256/suisho5` (cp 8 vs 94)。同じ最善手には収束しているが、
  この評価関数ではズレが評価値に出やすい

## Threads=8 のとき (非決定的)

「たくさんスレッドを使う」と速いが、**評価値が run ごとに変わる**。
KP256 (水匠 petite) で同一設定を3回:

| run | score | nodes |
|---|---|---|
| 1 | cp -1 | 1,551,315 |
| 2 | cp 24 | 2,199,548 |
| 3 | cp 37 | 1,139,065 |

同条件で **±40cp 程度ばらつく**（Threads=1 の cp 52 とも違う）。
評価関数同士を比較する目的なら Threads=1 を使うこと。

参考として Threads=8 / `USI_Hash 1024` / native での1回の測定値
(上の表とは hash が違うので直接比較はできない):

| 評価関数 | score | nodes | nps |
|---|---|---|---|
| `k_p_256/uonuma` | cp 136 | 2,285,451 | 2.71 M |
| `k_p_256/suisho` | cp -10 | 2,511,467 | 2.69 M |
| `halfkp_768/aoba` | cp -30 | 5,240,613 | 2.20 M |
| `halfkp_256/suisho5` | cp -18 | 3,198,972 | 2.88 M |
| `halfkp_256/hao` | cp -74 | 1,720,397 | 3.01 M |
| `halfka_hm2_1024/nagisa` | cp -38 | 1,936,221 | 1.78 M |

## エンジン側の確認結果

この計測のために4つのエディションをビルドした。**いずれも upstream V9.6x で
そのまま通り、fork 側の追加実装は不要だった。**

| 評価関数 | `YANEURAOU_EDITION` |
|---|---|
| KP256 系 | `YANEURAOU_ENGINE_NNUE_KP256` |
| HalfKP256 系 | `YANEURAOU_ENGINE_NNUE` |
| HalfKP768 (aoba) | `YANEURAOU_ENGINE_NNUE_halfkp_768x2_16_64` |
| SFNN (nagisa) | `YANEURAOU_ENGINE_SFNN_halfkahm2_1024_15_64_k3k3` |

意味のある2点:

1. **HalfKP768 が動的アーキ生成だけで通った。** fork が手で配線していた
   アーキヘッダと Makefile 分岐 (`17fcf51c`, `3e1f9068`) は不要になる。
   ビルド時に `PYTHON=python3` を渡す必要がある
2. **NAGISA_V3 の評価関数は「読める」が、本来の性能は出ていない。**
   → 詳細は下記「⚠ NAGISA_V3 の bucket 不一致」。

## ⚠ NAGISA_V3 の bucket 不一致 (この計測値は本来の強さではない)

**この表の `halfka_hm2_1024/nagisa` の数値は、本来と違う bucket 選択で
測ったものなので、NAGISA_V3 の実力を表していない。**

### 何が起きているか

このネットは LayerStack を 9 個持つ。9 個のうちどれを使うかを決める規則が
学習時と実行時で食い違っている。

| | bucket の決め方 |
|---|---|
| 学習時 (keinoda) | `progress8ek` = 進行度で 0〜7、相入玉局面なら 8 |
| 今回の計測 (upstream `k3k3`) | 玉の位置 (3x3 = 9通り) |

どちらも 9 バケットなのでネットワーク構造は同一で、**hash 検証を通ってしまう**。
つまりエラーも警告も出ないまま、学習時と別の重みが選ばれ続ける。

`progress8ek` の実装は keinoda 側の `source/tanuki_progress.cpp` にあり、
進行度モデルの係数を **`progress.bin` (別ファイル)** から読む:

```cpp
// keinoda/YaneuraOu source/tanuki_progress.cpp
int LayerStackIndexProgress8Ek(const Position& pos) {
    // 相入玉局面ではprogress係数を参照せず、9番目のLayerStackを選ぶ。
    if (IsMutualEnteringKing(pos)) return 8;
    return LayerStackIndex(pos);   // ← progress.bin の係数から進行度を計算
}
```

### upstream の進行度 bucket では代用できない

upstream HEAD にも進行度 bucket は入ったが、別物である。

| | upstream | keinoda (NAGISA_V3) |
|---|---|---|
| bucket 数 | 2 / 3 / 4 / 8 / 16 / 32 | 9 (進行度8 + 相入玉1) |
| 進行度係数の場所 | **`nn.bin` に埋め込み** (`evaluate_nnue.cpp:338` が同じ stream から読む) | **別ファイル `progress.bin`** |
| 相入玉の特別扱い | 無し | 有り (bucket 8) |

`YANEURAOU_ENGINE_SFNN_..._progress8` は 8 バケットなので 9 個のネットには
合わないし、係数を `nn.bin` から読もうとするので `progress.bin` は使われない。

### 本来の性能を出すには

keinoda 側の `tanuki_progress.{cpp,h}` (約 290 行) と、`progress.bin` を
`LS_PROGRESS_COEFF` で読ませる仕組みを移植する必要がある。
これは [`2026-07-27_nagisa_v3_diff_survey.md`](2026-07-27_nagisa_v3_diff_survey.md)
で「進行度SFNN」として整理した項目そのもので、**未着手**。

移植すれば upstream の SFNN 基盤 (LayerStack, HalfKA_hm2) の上に
bucket 選択規則を差し替えるだけで済むはずで、V8.50 時代に必要だった
「NNUE 基盤ごと移植」よりは大幅に小さい。

### WASM 化で見つかった不具合 2 件

**(a) `wasm_simd.cpp` の明示的インスタンス化不足**

`emscripten_wasm_simd::affine` は template だが定義が `.cpp` にしかないので、
使う層のサイズの組を明示的にインスタンス化しておく必要がある。
HalfKP768 と SFNN のサイズが無く、リンクエラーで落ちた。

```
wasm-ld: error: undefined symbol: emscripten_wasm_simd::affine<1024, 16, 1024>(...)
```

必要な組を追加して解決。エディションを増やすと再発するので、
`wasm_simd.cpp` にその旨をコメントで残した。**リンクエラーで気づけるので安全**。

**(b) explicit 層の重み配置が WASM で壊れる**

こちらは**黙って壊れる**ので厄介だった。SFNN は
`affine_transform_explicit.h` / `affine_transform_sparse_input_explicit.h` を使うが、
これらは snake_case の `get_weight_index` を持っており、
先に直した `GetWeightIndex` (`affine_transform.h` 等) とは別物だった。

結果、`Propagate()` の WASM SIMD 経路は重みを dense として読むのに、
読み込み時は scrambled 配置で並べる、という食い違いが残っていた。

| | nodes | score | bestmove |
|---|---|---|---|
| 修正前 | 7,500,088 | cp -20 | `7c6e ponder B*7c` |
| 修正後 | 389,416 | cp -54 | `3d3e ponder 2i3g` |
| native (参考) | 282,494 | cp -76 | `3d3e ponder 2i3g` |

**最善手が別物になっていた。** ビルドもリンクも通り、エンジンは正常に起動し、
それらしい評価値を返すので、比較して初めて気づけた。
NNUE 系エディションを WASM に載せるときは、必ず native と突き合わせること。

### ヘッダのアーキ文字列が実体と食い違う例

`halfkp_768/aoba` のヘッダは `Features=HalfKP(Friend)[125388->256x2]` と
名乗っているが、ファイルサイズ 183.7 MB は 768x2
(125,388 × 768 × 2 = 192,595,968 bytes) と一致する。実体は 768x2。

エンジンは `hash_value` で判定するのでこれで正しく読めている
(`0x3e5aa222`。256x2 の `0x3e5aa6ee` とは別値)。
ヘッダの文字列は表示用で、正しさの根拠にはならない。

## 再現方法

```sh
node script/bench_nodes.mjs \
  --eval assets/eval/<家系>/<名前>/nn.bin \
  --go "depth 16" --hash 256 --threads 1 \
  --native "label=<エンジンのパス>" --wasm "label=<WASMビルドのディレクトリ>"
```

評価関数は `assets/` 配下に置く (`.gitignore` 済み。リポジトリには入れない)。
