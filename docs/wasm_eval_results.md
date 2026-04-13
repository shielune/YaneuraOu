# WASM 評価値検証 — 結果表

最終更新: 2026-04-13

## 検証条件

- **SFEN**: `lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1`
- **Options**: `Threads=1`, `USI_Hash=64`
- **Command**: `go btime 0 wtime 0 byoyomi <think_ms>`
- **ランナー**: Playwright + Headless Chromium (`script/wasm_eval_browser.ts`)
- **YaneuraOu パッケージ**: `k-p` (K-P NNUE, tournament edition)

## ビルドステータス

| emscripten | アーキ    | ビルド | JS ファイル          | 備考 |
|-----------:|:---------|:-------|:---------------------|:-----|
| **3.1.43** | x86_64   | ✅ OK  | `.js` + `.wasm` + `.worker.js` | 旧スタイル: 別ファイル worker (classic script) |
| 3.1.50     | x86_64   | ❌ 失敗 | `.wasm` のみ          | メイン `.js` が欠落。`wasm_build.js` の圧縮ステップで途中終了した可能性。優先度低 |
| **3.1.60** | x86_64   | ⚠️ 壊れている | `.js` + `.wasm` | 生成されるが実行時エラー (pthread 判定) |
| **3.1.70** | x86_64   | ✅ OK  | `.js` + `.wasm`       | worker がメイン JS に統合 (ES module worker) |
| **3.1.74** | x86_64   | ⚠️ 壊れている | `.js` + `.wasm` | 生成されるが評価値は得られない (下記) |
| **4.0.0**  | x86_64   | ⚠️ 壊れている | `.js` + `.wasm` | 3.1.74 と同症状 |
| **4.0.11** | x86_64   | ⚠️ 壊れている | `.js` + `.wasm` | 3.1.74 と同症状 |
| **4.0.23** | aarch64  | ⚠️ 壊れている | `.js` + `.wasm` | ここから docker image が arm64 native (交絡要因注意) |
| **5.0.0**  | aarch64  | ⚠️ 壊れている | `.js` + `.wasm` | 3.1.74 と同症状 |
| **5.0.5**  | aarch64  | 💥 **JS 構文破綻** | `.js` + `.wasm` | 生成 JS にセミコロン抜け (`m=ba&&globalThis.name=="em-pthread"(function(){...`)、パース時に即 `TypeError` |

> `_x86_64` / `_aarch64` の接尾辞は `wasm_build.js` が `uname -m` をそのまま使っているだけで、wasm 自体は理想的にはアーキ非依存。ただし 4.0.23 から emscripten docker image が arm64 native になるので、切り分け時はそちらも疑うこと。

## 評価結果

### 5 秒探索 (`--think-ms 5000`, スモークテスト用)

| emscripten | スコア    | bestmove        | depth | nodes | 判定 |
|-----------:|:----------|:----------------|------:|------:|:-----|
| 3.1.43     | `cp 404`  | `G*9g 9h9g ...` | 20    | 2,762,915 | ✅ |
| 3.1.50     | —         | —               | —     | —     | N/A (ビルド失敗) |
| 3.1.60     | —         | —               | —     | —     | ❌ 実行時エラー |
| **3.1.70** | **`cp 404`** | **`G*9g 9h9g ...`** | **20** | **2,923,040** | ✅ **3.1.43 と一致** |
| 3.1.74     | —         | —               | —     | —     | ❌ `go` 後ワーカーから応答無し |
| 4.0.0      | —         | —               | —     | —     | ❌ 同上 |
| 4.0.11     | —         | —               | —     | —     | ❌ 同上 |
| 4.0.23     | —         | —               | —     | —     | ❌ 同上 |
| 5.0.0      | —         | —               | —     | —     | ❌ 同上 |
| 5.0.5      | —         | —               | —     | —     | 💥 ロード不可 |

### 30 秒探索 (`--think-ms 30000`, 本番判定用)

| emscripten | スコア    | bestmove        | depth | nodes      | time    | 判定 |
|-----------:|:----------|:----------------|------:|-----------:|--------:|:-----|
| **3.1.43** | **`cp 381`** | **`G*9g 9h9g ...`** | **24** | **22,863,926** | 28,883 ms | ✅ **BASELINE** |
| **3.1.70** | **`cp 381`** | **`G*9g 9h9g ...`** | **24** | **22,640,267** | 28,884 ms | ✅ **BASELINE と一致** |

PV (両バージョンで同一):
```
G*9g 9h9g 8a8g+ 8h8g G*8f 8g8f P*8e 8f9f N*8d 9f8e B*6g R*7f 6g7f+ 7g7f N*9c 8e7e R*8e 7e6f 4d5e 5f5e P*6e 6f7g S*8h 7g7h 8d7f G*3c
```

## 重要な発見

### 1. 3.1.43 と 3.1.70 は完全に同一の評価値を返す

30 秒固定探索で `cp 381 / G*9g / depth 24` まで完全に一致。nodes 数のわずかな差 (22.86M vs 22.64M) は主にタイマ割り込み由来の非決定性。**二分の対象は 3.1.70 以降**。

### 2. 3.1.74 以降は「評価値のずれ」ではなく「評価値が取得できない」状態

今回の検証で 3.1.74 / 4.0.x / 5.0.0 は WASM は生成されロードも通るが、`go` 実行後にワーカースレッドから一切出力が返ってこない (`workerMsgs delta=0`, `stdoutMsgs=32` の prelude のみ)。探索スレッドがそもそも起動していない可能性が高い。

実機のアプリでも同じ症状が出ているのであれば、**「新 emscripten では評価値が誤る」というよりも「そもそも検索エンジンが動かない」**のかもしれない。現物のフロントエンドで挙動を再確認する価値がある。

### 3. 5.0.5 は生成 JS 自体が壊れている

`grep` で直接確認したところ、以下のように `self.name=="em-pthread"` の後にセミコロンが抜けて `(function(){...})` を関数呼び出しとして繋げてしまっている:

```
m=ba&&globalThis.name=="em-pthread"(function(){function a(){var g=d.shift();...
```

これは `TypeError: "em-pthread" is not a function` で即死する。上流の minifier / closure バグと思われる。ビルドログを精査してどの段階でこうなったか追跡したい。

### 4. emscripten 3.1.74 以降の Module API 変更が YaneuraOu の `wasm_pre.js` を壊している

検証中に判明した範囲:

- **`var sa = console.log.bind(console)`** がハードコード。以前の `Module.print || console.log` のフォールバックが消滅。
- **`postRun`, `onRuntimeInitialized`, `preRun` は参照されない**。`wasm_pre.js` が `Module.postRun` にコマンドキューのフラッシュを登録しているが呼ばれない。
- **pthread ワーカーに送るプロキシハンドラリストが空配列** (`var d=[], e;for(e of d)...` という minified コード)。以前は `["onExit","onAbort","print","printErr"]` 相当が入っていた。
- `INCOMING_MODULE_JS_API` のデフォルト変更が根本原因と推定。`-s INCOMING_MODULE_JS_API=['print','printErr','postRun','preRun','onAbort','onExit',...]` を明示指定した再ビルドで直る可能性が高い。

### 5. 3.1.60 の「ワーカー pthread 判定」バグ

3.1.60 の minified JS では pthread 判定が `C=B&&"em-pthread"==self.name`、`B="function"==typeof importScripts` となっており、**ES module worker では `importScripts` が undefined なので常に false** → pthread 検出が機能しない。上流の既知バグ (3.1.70 で `WorkerGlobalScope` ベースに修正)。

### 6. pthread スレッドからのメイン スレッド`print` 転送の仕組み

新 emscripten の worker 側では、`y[g]` が falsy または `.proxy` プロパティを持つ場合のみポスト メッセージ proxy に置き換わる。`wasm_pre.js` の `Module.print = dispatcher` は truthy かつ `.proxy` 無しなので置き換わらず、ワーカー内の `listeners=[]` の空コールバックを通って **ワーカー自身の `console.log`** に落ちてしまう (メイン スレッドに届かない)。

## 今後のタスク

- [x] Playwright ベースの検証ランナー実装 (`script/wasm_eval_browser.ts`)
- [x] 3.1.43 / 3.1.70 ベースラインの取得 (5s / 30s)
- [x] 9 バージョン × k-p のスモーク実行
- [ ] **3.1.74 以降の復活**: `INCOMING_MODULE_JS_API` を明示指定して再ビルドし、評価値が取れるか確認
- [ ] **5.0.5 の構文破綻** の原因調査 (ビルドログ + wasm_pre.js 周辺)
- [ ] 3.1.74 と 3.1.70 の emscripten 側 changelog 差分調査
- [ ] 3.1.50 の単体再ビルド (`wasm_build.js` 修正含む)
- [ ] 評価値のずれが本当に発生しているのか、実機アプリでの再検証 (開発目的の根本確認)
- [ ] 他の k-p 以外のパッケージ (nnue-halfkp etc.) でも同様の傾向か確認

## 参考

- ランナー: `script/wasm_eval_browser.ts`
- ランナー HTML: `script/wasm_eval_runner.html`
- バッチ: `script/wasm_eval_all.sh`
- 旧 Node ランナー (3.1.43 のみ動作): `script/wasm_eval_test.mjs`, `script/wasm_eval_worker_shim.mjs`
- 計画 / 背景: `docs/wasm_eval_testing_plan.md`
