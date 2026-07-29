# WASM ビルドの性能測定 (V9.6x)

計測日: 2026-07-28 / ブランチ: `feature/upstream-v9.6x`

## 計測条件

| | |
|---|---|
| エンジン | `YANEURAOU_ENGINE_NNUE_KP256` |
| 評価関数 | 水匠 petite (`assets/eval/k_p_256/suisho/nn.bin`, 893,917 bytes) |
| 局面 | `l1g1k2nl/1r4g2/2nsppsp1/p1pp2p1p/1p4PP1/P1P2P2P/1PSPPS3/2GK1G1R1/LN5NL w Bb 32` |
| 探索 | `go nodes 1000000` |
| 固定 | `Threads=1` / `USI_Hash 256` / `usinewgame` (置換表クリア) / 定跡オフ / `PvInterval 0` |
| 試行 | 3回の中央値 |
| マシン | aarch64 10 core / emscripten 6.0.4 / Node 26 / Chromium 147 |

**絶対値はマシン依存**。意味があるのは同一マシン上での変種同士の比だけ。

`PvInterval 0` は必須。既定の 300ms 間引きのままだと、速い変種と遅い変種で
「最後に出力された info 行」が別の反復のものになり、探索は同一なのに
depth と score が食い違って見える。

## 1. 変種別スループット

| ビルド | 時間 | nps | 対 gcc scalar |
|---|---|---|---|
| native clang-14 (NEON) | 1,310 ms | 764k | 3.75× |
| native gcc (NEON) | 1,338 ms | 748k | 3.68× |
| **wasm32 pthread** | **1,448 ms** | **691k** | 3.40× |
| wasm64 pthread (Memory64) | 1,583 ms | 632k | 3.11× |
| wasm32 SIMD なし | 1,711 ms | 585k | 2.87× |
| native clang-14 (scalar) | 3,132 ms | 320k | 1.57× |
| native gcc (scalar) | 4,919 ms | 203k | 1.00× |

(この表は全 7 ビルドを 1 回の実行で計測したもの。`--json` で生データを残せる)

`USI_Hash 16` で揃えた edge 版 (pthread なし):

| ビルド | 時間 | nps |
|---|---|---|
| wasm32 edge | 1,313 ms | 762k |
| wasm32 pthread (同条件の参考値) | 1,328 ms | 753k |

### 読み取れること

- **WASM は native NEON の約 92%** の速度が出ている。`USE_WASM_SIMD` を外すと
  585k まで落ちる (15% 減) ので、WASM SIMD がきちんと効いている
- **edge 版 (pthread なし) は pthread 版と同等**。1スレッド動作では
  スレッド同期のオーバーヘッドが無いぶん、むしろわずかに速い
- **Memory64 は 9% 遅い**。バイナリも 872KB → 899KB (+3%)
- コンパイラの効き方が SIMD の有無で大きく違う:
  - NEON 版では gcc と clang の差が小さい (748k vs 764k、2%)。NNUE の重い部分が
    intrinsics で書かれていて、コンパイラの裁量が小さい
  - scalar 版では **clang が gcc より 57% 速い** (320k vs 203k)。手書き SIMD が
    無いぶん、自動ベクトル化の質がそのまま出る

## 2. スレッドスケーリング

| Threads | node nps | browser nps | browser / node |
|---|---|---|---|
| 1 | 686k | 678k | 0.99 |
| 2 | 1,433k (2.09×) | 1,386k (2.04×) | 0.97 |
| 4 | 2,916k (4.25×) | 2,678k (3.95×) | 0.92 |

- node 側は 4スレッドで 4.25× と、線形を超える。置換表の共有効果
- **ブラウザはスレッドを増やすほどわずかに不利**になる (4スレッドで 8% 差)。
  `SharedArrayBuffer` 経由のワーカー同期が `node:worker_threads` より
  やや重いためと思われるが、実用上は誤差の範囲

⚠ 2スレッド以上の探索は非決定的。この節は速度の比較にのみ使うこと。

## 3. node と browser で探索は一致する

1スレッド・同一条件で、両ランタイムの探索結果が完全に一致した。

```
node    : 1,000,322 nodes / cp -3 / bestmove 3d3e ponder 2i3g
browser : 1,000,322 nodes / cp -3 / bestmove 3d3e ponder 2i3g
```

ノード数まで一致するということは、探索木が同一で、途中の評価値もすべて
一致しているということ。ランタイムの違いはエンジンの読み筋に影響しない。

## 4. 探索結果のグループ分け

同じ条件で走らせたときに、どの変種が同じ木を読んでいるか。

| グループ | nodes | score | bestmove |
|---|---|---|---|
| wasm 全種 (32bit / 64bit / SIMD有無 / node / browser / edge) | 1,000,322 | cp -3 | `3d3e ponder 2i3g` |
| native scalar (gcc / clang) | 1,000,703 | cp -53 | `3d3e ponder 2i3g` |
| native NEON (gcc / clang) | 1,000,268 | cp 1 | `8b5b ponder 3e3d` |

**WASM 系は全て一致**する。ランタイムもポインタ幅も SIMD の有無も超えて
同一の探索をしているので、WASM ビルドの内部整合性は取れている。

一方で native との間、および native NEON と native scalar の間には差がある。
どちらも今回の移植が原因ではなく、調査の詳細は
[`docs/wasm_native_divergence.md`](../wasm_native_divergence.md) にまとめた。

## 再現方法

```sh
# node / native
node script/bench_nodes.mjs \
  --eval assets/eval/k_p_256/suisho/nn.bin \
  --nodes 1000000 --hash 256 --repeat 3 \
  --native "gcc_NEON=<path>" --wasm "wasm32=<dir>"

# browser (ヘッドレス Chromium)
node script/bench_browser.mjs --dir <web,worker ビルドのディレクトリ> \
  --eval assets/eval/k_p_256/suisho/nn.bin --nodes 1000000 --hash 256
```

`--go "depth 12"` でノード数指定の代わりに深さ指定にできる。
深さ指定のほうが変種間の比較には厳密 (打ち切り位置がぶれない)。

ブラウザ側は pthread が `SharedArrayBuffer` を要求するため、配信に
`Cross-Origin-Opener-Policy: same-origin` と
`Cross-Origin-Embedder-Policy: require-corp` が要る。ハーネスが付けている。

> 📝 `playwright install chromium` はこの環境では展開フェーズで停止した
> (ダウンロードは100%完了、FS は 500ファイル 9ms と高速、それでもファイル数が
> 10 のまま増えない)。zip を直接落として `unzip` すると 4 秒で 466 ファイルの
> 展開が終わったので、同じ症状に当たったら `--chromium <path>` で
> 手動展開先を指定すればよい。
