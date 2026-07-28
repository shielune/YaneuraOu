# V9.6x WASM ビルドの性能と、native との探索差異

計測日: 2026-07-28 / ブランチ: `feature/upstream-v9.6x`
エンジン: `YANEURAOU_ENGINE_NNUE_KP256` + 水匠 petite (`assets/eval/k_p_256/suisho/nn.bin`)

計測環境は aarch64 (10 core) / emscripten 6.0.4 / Node 26。
**絶対値はマシン依存**なので、意味があるのは同一マシン上での変種同士の比だけ。

再現コマンド:

```sh
node script/bench_nodes.mjs \
  --eval assets/eval/k_p_256/suisho/nn.bin \
  --nodes 1000000 --hash 256 --repeat 3 \
  --native "native=<path>" --wasm "wasm=<dir>"
```

---

## 1. 速度

局面 `l1g1k2nl/1r4g2/2nsppsp1/p1pp2p1p/1p4PP1/P1P2P2P/1PSPPS3/2GK1G1R1/LN5NL w Bb 32` を
100万ノード読ませたときの所要時間 (3回の中央値)。`Threads=1` / `USI_Hash 256` / `usinewgame`。

| ビルド | 時間 | nps | 対 gcc scalar |
|---|---|---|---|
| native clang-14 (NEON / GRAVITON2) | 1,346 ms | 743k | 3.70× |
| native gcc (NEON / GRAVITON2) | 1,350 ms | 741k | 3.69× |
| **wasm32 pthread** | **1,452 ms** | **689k** | 3.43× |
| **wasm64 pthread (Memory64)** | 1,637 ms | 611k | 3.04× |
| wasm32 SIMD なし | 1,760 ms | 568k | 2.83× |
| native clang-14 (scalar) | 3,202 ms | 313k | 1.56× |
| native gcc (scalar) | 4,984 ms | 201k | 1.00× |

コンパイラの効き方が SIMD の有無で大きく違う:

- **NEON 版では gcc と clang に差がない** (741k vs 743k)。NNUE の重い部分が
  intrinsics で書かれていて、コンパイラの裁量が小さいため
- **scalar 版では clang が gcc より 56% 速い** (313k vs 201k)。手書き SIMD が無い分、
  自動ベクトル化の質がそのまま出る

`USI_Hash 16` で揃えた場合の edge 版:

| ビルド | 時間 | nps |
|---|---|---|
| wasm32 edge (pthread なし) | 1,313 ms | 762k |
| wasm32 pthread (参考) | 1,328 ms | 753k |

要点:

- **WASM は native NEON の約 92%** の速度が出ている。WASM SIMD が効いている証拠で、
  `USE_WASM_SIMD` を外すと 568k まで落ちる (約 18% 減) ことからも裏付けられる。
- **edge 版 (pthread なし) は pthread 版と同等**。1スレッド動作ではスレッド同期の
  オーバーヘッドが無いぶん、むしろわずかに速い。
- Memory64 は 32bit 版比で **約 11% 遅い**。バイナリも 872KB → 899KB と少し大きい。

### スレッドスケーリング (wasm32 pthread)

| Threads | 時間 | nps | 対 1スレッド |
|---|---|---|---|
| 1 | 1,500 ms | 686k | 1.00× |
| 2 | 717 ms | 1,433k | 2.09× |
| 4 | 368 ms | 2,916k | 4.25× |

ほぼ線形。1倍を超えるのは置換表の共有効果。
⚠ 2スレッド以上の探索は非決定的なので、この列は速度の比較にのみ使うこと。

### node と browser の比較 — 未計測

`EM_ENVIRONMENT=web,worker` のビルドをヘッドレス Chromium で走らせる計測は
**この開発環境ではできなかった**。`~/.cache/ms-playwright/chromium-1217` の展開が
途中で切れており (`icudtl.dat` も `.pak` も無い)、起動時に
`Invalid file descriptor to ICU data received` で落ちる。入れ直しには
`npx playwright install` が要る。

計測用のハーネスは `script/bench_browser.mjs` に用意してある
(COOP/COEP 付きの静的サーバー + ページ側の USI ドライバ + Playwright 起動)。
**ブラウザが無いため未実行・未検証**なので、動くブラウザのある環境か CI で
最初に流すときは、まず動作確認から入ること。

```sh
node script/bench_browser.mjs --dir <web,worker ビルドのディレクトリ> \
  --eval assets/eval/k_p_256/suisho/nn.bin --nodes 1000000 --hash 256
```

pthread 版は `SharedArrayBuffer` を使うので、配信側に
`Cross-Origin-Opener-Policy: same-origin` と
`Cross-Origin-Embedder-Policy: require-corp` が必須。ハーネスはこれを付けている。

---

## 2. native と WASM で探索木が分かれる件

### 症状

同一条件 (`Threads=1` / TTクリア / 同一評価関数) でも、**局面によっては** native と WASM で
探索ノード数と途中の評価値が食い違う。bestmove は一致する。

| 局面 | depth 10 | depth 11 | depth 12 | depth 14 |
|---|---|---|---|---|
| 平手+`7g7f 3c3d 2g2f` | 一致 | 一致 | **一致** (69,661) | **一致** (174,669) |
| 上記のベンチ局面 | 一致 (37,456) | **乖離** (72,118 vs 61,076) | 乖離 | 乖離 |

### V8.50 では起きない

現行の出荷ベース (`develop` = upstream V8.50) では、同じ比較で
depth 8 / 10 / 12 / 14 のすべてがノード数まで完全一致する。
**この差異は upstream の V8.50 → V9.6x の変更で入ったもの。**

### 潰した仮説 (すべて実測)

| 仮説 | 検証 | 結果 |
|---|---|---|
| 置換表があふれた | `USI_Hash 512` で `hashfull 0` を確認 | ✗ 無関係 |
| ポインタ幅 32/64bit | `EM_MEMORY64=1` でビルドして比較 | ✗ 無関係 (32bit版と1ノードも違わない) |
| NNUE の WASM SIMD カーネル | `USE_WASM_SIMD` を外してビルド | ✗ 無関係 (SIMD 有無で同一結果) |
| コンパイラの差 | native を gcc と clang-14 でビルド | ✗ 無関係 (両者完全一致) |
| `-ffast-math` | フラグを外して native をビルド | ✗ 無関係 |
| FMA 積和融合 (ARM にあり wasm に無い) | `-ffp-contract=off` で native をビルド | ✗ 無関係 |
| `std::log` の実装差 → reductions テーブル | 両環境で i=1..255 を実測して突合 | ✗ 無関係 (全一致) |
| 指し手ソートの同値順 (libstdc++ vs libc++) | 実装を確認 | ✗ 無関係 (両版とも自前の insertion sort) |
| 非決定性 | 各ビルドを3回実行 | ✗ 無関係 (毎回同一) |

構図としては **WASM 系 (32bit/64bit/SIMD有無) が全て一致**し、
**native scalar 系 (gcc/clang) が全て一致**し、その2陣営の間でだけ食い違う。

### 別件: native NEON も scalar と食い違う

上とは独立した現象として、`TARGET_CPU=GRAVITON2` (NEON) のビルドは
native scalar とも WASM とも違う結果を返す。

| ビルド | nodes | score | bestmove |
|---|---|---|---|
| gcc NEON / clang NEON | 1,000,268 | cp 1 | `8b5b ponder 3e3d` |
| gcc scalar / clang scalar | 1,000,703 | cp -53 | `3d3e ponder 2i3g` |
| wasm (全変種) | 1,000,322 | cp -3 | `3d3e ponder 2i3g` |

**gcc と clang の NEON 版が完全一致する**ので、コンパイラのコード生成ではなく
**NEON カーネルのコードそのもの**が scalar と違う答えを出している。

この差異は V8.50 にも存在し、今回の移植とは無関係。怪しい点として
`USE_NEON_DOTPROD` が `affine_transform.h` など9箇所で参照されているのに
**ソースのどこでも定義されていない**ことを確認している
(`TARGET_CPU=GRAVITON2` は `-march=...+dotprod` でコンパイルしているのに、
コード側は dotprod 無しとして扱われる)。

配布物は WASM と x86 中心なので実害は限定的だが、ARM ネイティブ
(Apple Silicon 含む) でビルドして使う場合は影響しうる。

### 現時点の評価

実害は薄いと考えている。根拠:

1. 評価関数の計算は一致している。深さ10まで (別局面では14まで) ノード数までビット一致するので、
   NNUE の出力が違っていればもっと浅い段階で差が出るはず
2. WASM 側は 32bit/64bit/SIMD有無を問わず自己整合的で、実行のたびに同じ結果を返す
3. 乖離する局面でも bestmove は一致している
4. 系統的でなく局面依存 — どこかの閾値比較が境界に乗ったときだけ倒れる類に見える

ただし V8.50 で保たれていた一致が失われたのは事実で、
「upstream の新しい探索機構のどこかに環境で結果が変わる箇所がある」ことは確定している。

### 未着手の追跡方法

探索の各ノードで `(ply, depth, move, value)` を出力させ、native と WASM のログを
diff して**最初に食い違う1行**を特定する。ビルド2回と差分取りで済み、原因コードまで届く。
upstream へ報告する材料が必要になった時点で実施する。

### 移植作業への影響

「native と WASM のノード数一致」は移植の検証手段として強力だったが、V9.6x では
局面によって使えない。代わりに以下で担保する:

- **WASM 変種同士の一致** (pthread / edge / 32bit / 64bit) — これは現在も完全に成立する
- **一致する局面での native 比較** (平手+3手の局面は depth 14 まで一致)

---

## 3. Memory64 (wasm64) について

`EM_MEMORY64=1` でポインタ64bitのビルドができる (`source/Makefile` の em++ ブロック)。

```sh
make tournament COMPILER=em++ TARGET_CPU=WASM \
  YANEURAOU_EDITION=YANEURAOU_ENGINE_NNUE_KP256 \
  EM_MEMORY64=1 EM_PTHREAD=1 ...
```

- 4GB のヒープ上限を超えられる。巨大な評価関数 (halfka_hm2 の 78MB クラス) と
  大きな置換表を同居させたいときに効く
- 速度は 32bit 版比で約 11% 遅く、バイナリは約 3% 大きい
- **実行側の要件が上がる**: WebAssembly Memory64 に対応したランタイムが必要。
  Node 26 で動くことは実測で確認した。**それ以外のランタイム (ブラウザ各種、
  Cloudflare Workers など) の対応状況は未確認**なので、公開パッケージに載せる前に
  ターゲットごとの確認が要る

既定は `EM_MEMORY64=0`。公開パッケージは 32bit のままにしてある。
