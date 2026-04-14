#  WASM 評価値検証 — 4.0.0 以降の不動作レポート

最終更新: 2026-04-14

## TL;DR

- **emscripten 3.1.70 までは 3.1.43 とビット単位で同じ評価値** (`cp 381 / G*9g / depth 24`) を返す。
- **3.1.74 〜 5.0.0 は「評価値のズレ」ではなく「探索結果が一切返ってこない」。** WASM ロードは通るが `go` コマンド後、ワーカーから `info` / `bestmove` が出力されない。
- **5.0.5 は生成 JS そのものが構文エラー** で `<script>` 読み込み時点で即死する。
- 現時点でのフロントエンド側観測 (「新 emscripten で評価値が誤る」) は、再検証すると**そもそも探索自体が走っていない**可能性が高い。誤評価ではなく起動不良。
- 原因の第一容疑は **`INCOMING_MODULE_JS_API` のデフォルト変更** (3.1.74 で `print` / `printErr` / `postRun` / `preRun` が外された) で、`wasm_pre.js` が前提としている `Module.postRun` 登録・`Module.print` 差し替えが無視されるようになった。

## 検証条件

| 項目 | 値 |
|:--|:--|
| テスト局面 (SFEN) | `lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1` |
| Options | `Threads=1`, `USI_Hash=64` |
| コマンド | `go btime 0 wtime 0 byoyomi <think_ms>` |
| パッケージ | `k-p` (K-P NNUE, tournament edition) |
| ランナー | Playwright + Headless Chromium (`script/wasm_eval_browser.ts`) |
| 静的サーバ | COOP/COEP ヘッダ付きで `build/<ver>/<pkg>/lib` を配信 |

## バージョン別ステータス一覧

| emscripten | ホストアーキ | ビルド | ロード | 探索実行 | 備考 |
|-----------:|:------|:--|:--|:--|:--|
| 3.1.43 | x86_64  | ✅ | ✅ | ✅ | 旧スタイル classic worker。ベースライン |
| 3.1.50 | x86_64  | ❌ | — | — | メイン `.js` が生成されない。優先度低 |
| 3.1.60 | x86_64  | ✅ | ❌ | — | `Ga is not a function` で即死 (下表 A) |
| 3.1.70 | x86_64  | ✅ | ✅ | ✅ | `3.1.43` と評価値完全一致 |
| **3.1.74** | x86_64  | ✅ | ✅ | ❌ | `go` 後ワーカー無応答 (下表 B) |
| **4.0.0**  | x86_64  | ✅ | ✅ | ❌ | 3.1.74 と同症状 |
| **4.0.11** | x86_64  | ✅ | ✅ | ❌ | 3.1.74 と同症状 |
| **4.0.23** | aarch64 | ✅ | ✅ | ❌ | 3.1.74 と同症状。docker image が arm64 native に切替 |
| **5.0.0**  | aarch64 | ✅ | ✅ | ❌ | 3.1.74 と同症状 |
| **5.0.5**  | aarch64 | ⚠️ | ❌ | — | 生成 JS が構文破綻 (下表 C) |

> `_x86_64` / `_aarch64` の接尾辞は `wasm_build.js` が `uname -m` をそのまま使っているだけで、wasm 自体は理想的にはアーキ非依存。ただし 4.0.23 から docker image が arm64 native になるので、交絡要因として記録しておく。

## 動作している版の評価値 (参考)

### 30 秒探索

| emscripten | score | bestmove | depth | nodes | time |
|-----------:|:--|:--|--:|--:|--:|
| 3.1.43 | `cp 381` | `G*9g 9h9g …` | 24 | 22,863,926 | 28,883 ms |
| 3.1.70 | `cp 381` | `G*9g 9h9g …` | 24 | 22,640,267 | 28,884 ms |

nodes 数のわずかな差 (22.86M vs 22.64M) はタイマ割り込み由来の非決定性。**二分対象は 3.1.70 以降**。

## 4.0.0 以降で何が起きているか

### 症状 B — `go` 後ワーカー完全無応答 (3.1.74 / 4.0.0 / 4.0.11 / 4.0.23 / 5.0.0)

Playwright で観測された挙動を、エンジン初期化から順に並べたもの。

| フェーズ | 3.1.70 (正常) | 3.1.74 以降 (異常) |
|:--|:--|:--|
| `import()` でメイン JS 読み込み | ✅ | ✅ |
| `wasmModule` / `wasmMemory` の生成 | ✅ | ✅ |
| pthread ワーカー起動 | ✅ | ✅ |
| `stdout` に prelude (USI 初期メッセージ) 到達 | ✅ 32 行 | ✅ 32 行 |
| `isready` → `readyok` | ✅ | ✅ |
| `position sfen ...` | ✅ (ACK) | ✅ (ACK) |
| `go btime 0 wtime 0 byoyomi 5000` | ✅ | ⚠️ 送信は成功 |
| `info score cp ...` が届く | ✅ 逐次 | ❌ **0 行** |
| `bestmove …` が届く | ✅ | ❌ **タイムアウト** |
| `workerMsgs delta` (メインスレッドへのポスト) | ✅ 多数 | ❌ **0 件** |

つまり「エンジンは起動し、コマンドも受け付け、go も投げ終わっているのに、ワーカー側で走っているはずの探索スレッドからの stdout が 1 行もメインスレッドに届かない」。

**根本原因の仮説:**

| # | 観測 | 解釈 |
|:-:|:--|:--|
| 1 | minified main JS 内に `var sa = console.log.bind(console)` がハードコード。旧版の `Module.print \|\| console.log` フォールバックが消滅 | `Module.print = ...` で差し替えても参照されない |
| 2 | `postRun` / `onRuntimeInitialized` / `preRun` が main JS 内で参照されない | `wasm_pre.js` が `Module.postRun` に積んでいるコマンドキューのフラッシュが**永久に呼ばれない** |
| 3 | pthread ワーカー側のプロキシハンドラリストが空配列 (`var d=[], e; for(e of d) …`)。旧版は `["onExit","onAbort","print","printErr"]` 相当が入っていた | ワーカーで発生した `print` / `printErr` がメインスレッドに転送されず、ワーカーの `console.log` に吸い込まれる |
| 4 | エンジン main loop はワーカー側で回っているが、そこで実行される `std::cout << "info ..."` は `printf` hook → emscripten の `out()` → **ワーカー内** `console.log` に落ちる | ブラウザのメインスレッドから見ると一切の出力が無い |

これらは単一の上流変更で説明が付く: **emscripten 3.1.74 で `INCOMING_MODULE_JS_API` のデフォルトから `print` / `printErr` / `postRun` / `preRun` が外された**こと。Closure/minifier が「どうせ外から来ない」と判断して関連ハンドラをデッドコード除去しているため、`wasm_pre.js` 側でいくら設定しても効かない。

**暫定対処 (813bc30b で適用):** `source/Makefile` のリンクフラグに

```
-sINCOMING_MODULE_JS_API=['print','printErr','postRun','preRun','onAbort','onExit','onRuntimeInitialized','wasmBinary','noExitRuntime','locateFile','instantiateWasm']
```

を明示指定。再ビルドして症状が消えるかを次の検証タスクで確認する。

### 症状 C — 生成 JS の構文破綻 (5.0.5)

`grep` で直接確認したところ、minified 出力に以下のパターンがある:

```js
m=ba&&globalThis.name=="em-pthread"(function(){function a(){var g=d.shift();...
```

これを最小再現すると、

| 期待される形 | 実際の出力 |
|:--|:--|
| `m = ba && globalThis.name == "em-pthread";  (function(){ … })();` | `m=ba&&globalThis.name=="em-pthread"(function(){ … })` |

`"em-pthread"(function(){...})` は **文字列リテラルを関数として呼び出す** 式になり、実行時に即 `TypeError: "em-pthread" is not a function` で落ちる。

これは emscripten 側の minifier バグと思われる。現状は再現報告の準備のために以下を記録:

| 確認事項 | 状態 |
|:--|:--|
| 5.0.5 docker image (`emscripten/emsdk:5.0.5`) の SHA | 未取得 |
| `emcc -v` ログ | `build/multibuild_logs/5.0.5_k-p.log` 参照 |
| 同一ソースを 5.0.0 で通した差分 | 5.0.0 は症状 B (起動不良)、構文は正常 |
| 上流 issue | 未調査 |

### 参考: 3.1.60 の別バグ (症状 A)

3.1.74 以降とは無関係だが、中間バージョン検証の邪魔になるので記録しておく。3.1.60 の minified JS では pthread 判定が

```js
C = B && "em-pthread" == self.name;
B = "function" == typeof importScripts;
```

となっており、**ES module worker では `importScripts` が undefined なので `B` が常に false** → pthread 検出そのものが機能しない。3.1.70 で `WorkerGlobalScope` ベースの判定に修正されており、これは上流で既に解決済みの既知バグ。

## 現物アプリの「評価値がずれる」報告との突合

当初の問題提起は「新 emscripten にすると評価値がおかしくなる」というものだった。しかし今回の検証では:

| バージョン | 評価値が **ずれる** | 評価値が **出ない** |
|:--|:-:|:-:|
| 3.1.43 〜 3.1.70 | ❌ (baseline と一致) | ❌ |
| 3.1.74 以降 | — | ✅ **こちら** |

**「新しい emscripten で評価値が誤る」というより「そもそも探索エンジンが起動していない」可能性が高い。** フロントエンドが `bestmove` を待たずに `cp` の途中値を表示しているだけなら、見た目は「とんでもない手を選ぶ評価の狂ったエンジン」に見える。本体アプリでの再検証が必要。

## 今後のタスク

- [ ] **3.1.74 で `INCOMING_MODULE_JS_API` を明示指定した再ビルド**の結果確認 (813bc30b の検証)
- [ ] 直ったら 4.0.0 / 4.0.11 / 4.0.23 / 5.0.0 に横展開
- [ ] 5.0.5 の構文破綻について上流 issue を検索、見つからなければ最小再現を作って報告
- [ ] 本体アプリ (モバイル実機) で「評価値が出ていないのか、誤っているのか」を確認
- [ ] 3.1.74 で直った後、改めて 30 秒探索で `cp 381 / G*9g / depth 24` と一致するか確認 (回帰の最終ゲート)
- [ ] 3.1.50 の main `.js` 欠落再ビルド (優先度低)

## 参考ファイル

- ランナー: `script/wasm_eval_browser.ts`
- ランナー HTML: `script/wasm_eval_runner.html`
- バッチ: `script/wasm_eval_all.sh`
- 計画 / 背景: `docs/wasm_eval_testing_plan.md`
- ビルドログ: `build/multibuild_logs/<ver>_k-p.log`
