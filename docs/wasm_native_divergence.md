# native と WASM で探索結果が分かれる件

upstream V9.6x への追従作業中に見つけた差異の調査記録。
性能の数値は [`reports/2026-07-28_wasm_v96x_performance.md`](reports/2026-07-28_wasm_v96x_performance.md)、
移植作業そのものは [`reports/2026-07-28_wasm_v96x_port.md`](reports/2026-07-28_wasm_v96x_port.md) を参照。

計測条件は上記レポートと同じ (`Threads=1` / TTクリア / `PvInterval 0` /
KP256 + 水匠 petite)。

### 症状

同一条件 (`Threads=1` / TTクリア / 同一評価関数) でも、**局面によっては** native と WASM で
探索ノード数と途中の評価値が食い違う。bestmove は一致する。

| 局面 | depth 10 | depth 11 | depth 12 | depth 14 |
|---|---|---|---|---|
| 平手+`7g7f 3c3d 2g2f` | 一致 | 一致 | **一致** (69,661) | **一致** (174,669) |
| `l1g1k2nl/1r4g2/...` (ベンチ局面) | 一致 (37,456) | **乖離** (72,118 vs 61,076) | 乖離 | 乖離 |

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
