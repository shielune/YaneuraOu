#  WASM 評価値検証 — 全バージョン dual-runner 動作確認レポート

最終更新: 2026-04-14 (V7.61 再 baseline / emscripten 5.0.0 固定)

## 2026-04-14 追補: V7.61 downgrade 後の baseline 更新

前セッションの bisection で upstream YaneuraOu 本体のバージョン V8.50 以降
(`74d9b0e9 V8.50` と後続の V9.xx 系) で WASM の評価値が壊れることが判明した
ため、`a7229610 feat(wasm): downgrade YaneuraOu source to V7.61` で develop
を V7.61 (known-good state) に pin した。

**以下の旧 TL;DR 以降のセクションの数値はすべて V8.50 以降の壊れた state
で記録されたもの**で、現在の develop では再現しない。新 baseline は V7.61
エンジンが返す `cp 1500 帯 / B*6f` 方向で、engine id は
`id name YaneuraOu NNUE KP256 7.61 32WASM TOURNAMENT`。

- **emscripten ターゲットは 5.0.0 で固定**。5.0.5 は下の症状 C
  (`"em-pthread" is not a function`) が upstream 未解決のため、
  今後の upstream minifier 修正待ちで保留する。次期バージョン (5.0.6 以降)
  の追従も現在は行わない。
- **30 秒探索の V7.61 新 baseline**
  (`bun script/wasm_eval_{node,browser}.ts <path> --think-ms 30000`,
  2026-04-14 再計測):

| variant | score | bestmove | depth | nodes | time |
|---|---|---|---|---|---|
| 5.0.0 node    | `cp 1658 (lowerbound)` | `B*6f ponder S*3c` | 22 | 23,329,254 | 27,727 ms |
| 5.0.0 browser | `cp 1658 (lowerbound)` | `B*6f ponder S*3c` | 22 | 24,873,913 | 26,374 ms |
| 5.0.5 node    | `cp 1545 (lowerbound)` | `B*6f ponder S*3c` | 22 | 20,447,573 | 26,248 ms |
| 5.0.5 browser | — (症状 C: minifier バグで起動不可) | — | — | — | — |

30 秒でも `lowerbound` が付くのは、V7.61 エンジンが aspiration window 下限
に到達した時点で時間切れになっているため。探索時間を伸ばせばさらに上方に
振れる余地があるが、**`bestmove B*6f (ponder S*3c)` は全 variant で完全
一致**しており、これが V7.61 エンジンの正解方向と確定した。

- **`script/wasm_build.js` に `patches/` 自動適用フックを追加**
  (`6addf5f0 build(wasm): add idempotent patches/ auto-apply hook to wasm_build.js`)。
  現時点では `patches/` 配下は空で no-op だが、将来 emscripten 固有の
  source patch が必要になった時のインフラとして残している。ビルド前に
  unified diff を apply、終了時 (成功/失敗問わず) に逆 apply。

## 2026-04-14 追補 (続): YaneuraOu V8.50 まで到達 / V9.x 系は upstream 待ち

同日に `a7229610 downgrade to V7.61` の上に upstream YaneuraOu 本体のバージョン
を段階的に引き上げて再検証。emscripten は **5.0.0 固定** のまま。結論は
**develop を V8.50 (`74d9b0e9`) まで上げて一旦停止**。V9.00 以降は upstream
側の事情で追従不可と判明。

### V7.61 → V8.50 の段階反映 (通った)

develop に FF merge 済みの commit:

| commit | description |
|---|---|
| `0b8a477b` | `feat(wasm): upgrade YaneuraOu source to V7.73r (1fcbef31)` |
| `e62a47a2` | `feat(wasm): upgrade YaneuraOu source to V8.50 (74d9b0e9) + SF17 scrambled weight fix` |

V7.61 → V7.62 → V7.63 → V7.73r → V8.50 の順に `git checkout <ver> -- source/`
で source tree を書き換え、各段階で `script/wasm_build.js k-p` と
`wasm_build.js halfkp` を 5.0.0 で build、`wasm_eval_node.ts` +
`wasm_eval_browser.ts` を 30 秒 byoyomi で回して bestmove `B*6f` 方向を
維持していることを確認した。

V8.50 の段階で 2 つの WASM 特有の修正が必要だった:

1. **Makefile の em++ block 再アップデート** — upstream V8.50 の
   `source/Makefile` が wasm-eval-runner 側のフラグ (`EXPORT_ES6=1`、
   `ENVIRONMENT`/`EXPORTED_RUNTIME_METHODS` の変数化、
   `INCOMING_MODULE_JS_API` の明示 whitelist) を持っていなかったので、
   develop 側に蓄積していた flag 群を V8.50 em++ block に再移植した。
2. **SF17 scrambled weight layout の WASM 短絡** — commit 9c41f5b7
   (Stockfish17 AffineTransform 移植) + 434a3392 (ClippedReLU AVX-512)
   の組み合わせで、`GetWeightIndexScrambled()` 経由の重み配置が
   `USE_SSSE3 || USE_NEON_DOTPROD` 下で有効になる。em++ ブロックの
   `-DUSE_SSE42` が `config.h` 経由で `USE_SSSE3` まで連鎖するため WASM
   ビルドも scrambled layout で重みを保存するのに、既存の
   `USE_WASM_SIMD` ショートカット (`Propagate()` 内) が dense row-major
   として読むせいで K-P 3 層の NNUE 出力が ~1200 cp 低く腐り、bestmove
   が `8a8g+` に飛ぶ症状が出ていた。
   `source/eval/nnue/layers/affine_transform.h` と
   `affine_transform_sparse_input.h` の `GetWeightIndex()` に
   `#if defined(USE_WASM_SIMD) return i;` の分岐を追加して dense を強制し、
   bestmove を `B*6f` に戻した。

V8.50 の smoke (30 秒 byoyomi, Threads=1, USI_Hash=64, k-p / halfkp 両方):

| pkg | runner | score | bestmove | depth |
|---|---|---|---|---|
| k-p    | node    | `cp 1290`    | `B*6f ponder 3d3c+` | 24 |
| k-p    | browser | `cp 756 lb`  | `B*6f ponder 3d3c+` | 27 |
| halfkp | node    | `cp 1180`    | `B*6f ponder 3d3c+` | 26 |
| halfkp | browser | `cp 1146`    | `B*6f ponder 3d3c+` | 29 |

bestmove `B*6f` は V7.61 〜 V8.50 で不変。`cp` 値は upstream の探索パラメータ
調整と reachable depth の変化で絶対値は動くが、PV も含めて同じ方向を指す。

### V9.00 / V9.10 / V9.22 は upstream 都合で追従不可

V9.00 (`a5ee2786`) / V9.10 (`08f7d99b`) / V9.22 (`9c5b226e`) はどれも
`source/usi.cpp` の `#if defined(__EMSCRIPTEN__)` ブロック全体が
**`#if 0  // TODO : 🚧 工事中 🚧`** の内部に埋め込まれている。したがって
`usi_command` export 自体が wasm に出ず、`ccall("usi_command", ...)` が
`func is not a function` で落ちる。

`#if 0` を外せば直るわけではない:

- `#if 0` を追加したのは `cc78f439`(V9.00beta 途中) だが、その 1 つ前の
  `0ae12b89` (= `cc78f439^`) を試した時点で既に `usi.cpp:1729` 付近の
  `usi_command` が V9.00 の新 API と不整合で compile fail する。具体的には:
  - `Threads` (global) が削除 / Engine class 内の ThreadPool に移動
  - `Thread::threadStarted` member 削除
  - `usi_cmdexec(pos, states, cmd)` (free function) が
    `USIEngine::usi_cmdexec(const std::string&)` member method に rename
- つまり `cc78f439` は原因ではなく、upstream が V8.50→V9.00 の大規模
  Engine class refactor (`b3ff1749`, `e32ed51b` 等) で壊れた WASM 対応を
  build error から救うために `#if 0` でラップした「結果」コミット。
- 追従するなら `usi_command` を V9.x の新 API に書き直す必要があり、
  かつ `run_engine_entry()` で作られる `USIEngine` instance を global
  からアクセスできる設計 (WASM 専用 main、instance の静的登録、等) を
  入れないと WASM 環境からは command を送る手段がない。これは独自 fork
  レベルの継続コストなので、**upstream が自前で WASM 対応を refactor
  し終えるまで V9.x は見送り**。

### このセッションで確定した固定点

- **emscripten ターゲットは 5.0.0 固定** (5.0.5 は browser 側の
  `"em-pthread" is not a function` minifier バグで保留、次期 emscripten
  バージョンも追わない)。
- **YaneuraOu source は V8.50 + SF17 scrambled fix 固定**
  (develop HEAD `e62a47a2`)。
- V7.61 の新 baseline (冒頭「2026-04-14 追補」の表) は **V8.50 上の新
  baseline (上表) に更新**。bestmove は `B*6f` で一貫、score は V8.50 で
  より深く探索した分ブレる。

---

以下の旧 TL;DR 以降のセクションは V8.50 以降の「壊れた state」時代の記録。
emscripten バージョン互換性の知見 (症状 B = 3.1.74 以降の Module.print
dead-code elimination / 症状 A = 3.1.60 pthread 検出バグ / 症状 C = 5.0.5
minifier 出力の構文破綻 / Node 側 3 バージョンの pthread stall) は V7.61
でも変わらないので歴史として保持するが、**評価値の数値 (`cp 381` / `cp 404`
/ `cp 417` / `cp 577` / `bestmove G*9g` / `bestmove G*6h`) は V7.61 では
再現しない**。

## TL;DR (2026-04-14 再検証)

**ほぼ全バージョンが復旧した。** `source/Makefile` の
`INCOMING_MODULE_JS_API` 明示 whitelist + 2 変種ビルド (web/node の
env / exports を変える) + `script/loaders/` の per-generation ローダー
の 3 点セットで、Playwright 経路は 3.1.74 以降も復旧し、Node 経路も
4.0.11 以降は問題なく動く。

**2 局面で smoke** — 下表は 局面 A / 局面 B の bestmove を併記。

| emscripten | node | browser |
|---|---|---|
| 3.1.43 | ✅ `G*9g` / `G*6h` | ✅ `G*9g` / `G*6h` |
| 3.1.70 | ❌ stall | ✅ `G*9g` / `G*6h` |
| 3.1.74 | ❌ stall | ✅ `G*9g` / `G*6h` |
| 4.0.0  | ❌ stall | ✅ `G*9g` / `G*6h` |
| **4.0.11** | ✅ `G*9g` / `G*6h` | ✅ `G*9g` / `G*6h` |
| **4.0.23** | ✅ `G*9g` / `G*6h` | ✅ `G*9g` / `G*6h` |
| **5.0.0**  | ✅ `G*9g` / `G*6h` | ✅ `G*9g` / `G*6h` |
| **5.0.5**  | ✅ `G*9g` / `G*6h` | ❌ minifier bug |

(局面 A = baseline `lr5nl/…/LK3G3/8L`, 局面 B = `lr5nl/…/3K4L`)

- **28 / 32 ケース PASS** (両局面の合計)。bestmove は PASS 全ケース
  で完全一致。
- 残る 4 FAIL は Node 3 バージョン(3.1.70/3.1.74/4.0.0)× 2 局面と、
  5.0.5 browser × 2 局面のうち **既知の 2 つのみ**(Node stall は
  pthread 由来 / 5.0.5 browser は emscripten 自身の minifier バグ)。
- 評価値は node と browser で一貫した差があるが、これは探索進行速度
  の違いで bestmove は同じ。emscripten バージョンによる評価関数の
  ずれは両局面で検出されなかった。

- **Browser は 5.0.5 を除いて全バージョンで動作**。5.0.5 は生成 JS
  のまま `"em-pthread"(function(){…})` という minifier バグで
  `<script>` パース時に即死する(上流に報告が必要)。
- **Node も 4.0.11 以降は動作**。3.1.70 / 3.1.74 / 4.0.0 の 3
  バージョンだけが `ccall-only/node` loader でも stall する(未解決)。
  stall 中は `engine.ccall("usi_command", ..., ["usi"])` が永続的に
  `1` (busy) を返し、`postRun` が発火せず、`wasm_pre.js` の queue も
  drain されない。`script/loaders/external_queue.ts` pump を使っても
  同じ。pthread 層の問題と推定(詳細は「Node 側 3 バージョン stall の
  未解決問題」節)。
- **5 秒探索での評価値は全バージョンで一致**(cp 404 または cp 417、
  探索深度依存)、**bestmove はすべて `G*9g`**。
- **検証パイプラインは Node と Playwright の 2 経路の dual-runner**
  (`script/wasm_eval_node.ts` + `script/wasm_eval_browser.ts`) を
  `script/wasm_eval_all.sh` から同時実行する。各ランナーは
  `script/loaders/{node,browser}/` 配下のローダーで generation ごとの
  差を吸収する。

## 以前の症状の扱い

このドキュメントはもともと「4.0.0 以降の不動作レポート」として始まった。
元の症状(全バージョンで `go` 後に応答なし)は **2026-04-14 時点で
Browser では全バージョン解消済み**。以下の 4 つの fix がそれぞれ
貢献している:

1. **`source/Makefile` に `-s INCOMING_MODULE_JS_API=print,printErr,postRun,preRun,…` を明示** (commit 813bc30b)
   3.1.74 で emscripten が `INCOMING_MODULE_JS_API` のデフォルトから
   `print`/`printErr`/`postRun`/`preRun` を落としたため、Closure
   minifier が `Module.print` 経路を dead-code eliminate していた。
   明示 whitelist で復活。
2. **`script/wasm_eval_browser.ts` の静的サーバで pthread prelude を engine JS に注入**
   3.1.74 で pthread worker の proxy handler list が空になり、worker
   側 `console.log` が親に届かなくなった。Prelude で `console.log` を
   `postMessage({__yaneurao_stdout, text})` に差し替え、メインスレッド
   で `window.Worker` を patch して回収する。
3. **`script/loaders/browser/common.ts` の `installConsoleTap` + `installWorkerStdoutTap`**
   3.1.74 以降の minified main JS は `Module.print` を完全に無視し、
   `console.log` を直叩きするため、browser 側の loader で
   `console.log` と `window.Worker` を両方 tap して回収する。
4. **`source/Makefile` の em++ block を変数化 + 2 variants build**
   `EM_ENVIRONMENT` / `EM_EXPORTED_RUNTIME_METHODS` を make variable
   にして、`script/wasm_build.js` が web (ENVIRONMENT=web,worker /
   FS+ccall) と node (ENVIRONMENT=node / FS+ccall+callMain) の 2
   artefact を 1 ソースから吐く。

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

## dual-runner smoke test (2026-04-14 全バージョン)

新しい 2-variant ビルド (`build/<ver>_<arch>/k-p/{web,node}/lib/`)
に対して `script/wasm_eval_all.sh` を `--think-ms 5000` で走らせた結果。
Loader は `script/loaders/detect.ts` が自動で選択する(classic-worker
は 3.1.43 のみ、esmodule-worker は 3.1.44 ≤ v < 3.1.74、ccall-only
は v ≥ 3.1.74)。

2 つの独立な検証局面で smoke した:

### 検証局面 A (baseline)

```
lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1
```

結果(`build/eval_results_20260414_083156.jsonl`):

| emscripten | アーキ    | node (ccall-only/node 等) | browser |
|-----------:|:----------|:--|:--|
| 3.1.43 | x86_64  | ✅ `cp 417 / G*9g` | ✅ `cp 404 / G*9g` |
| 3.1.70 | x86_64  | ❌ `usi timeout` | ✅ `cp 404 / G*9g` |
| 3.1.74 | x86_64  | ❌ `usi timeout` | ✅ `cp 404 / G*9g` |
| 4.0.0  | x86_64  | ❌ `usi timeout` | ✅ `cp 417 / G*9g` |
| 4.0.11 | x86_64  | ✅ `cp 417 / G*9g` | ✅ `cp 404 / G*9g` |
| 4.0.23 | aarch64 | ✅ `cp 417 / G*9g` | ✅ `cp 404 / G*9g` |
| 5.0.0  | aarch64 | ✅ `cp 417 / G*9g` | ✅ `cp 404 / G*9g` |
| 5.0.5  | aarch64 | ✅ `cp 417 / G*9g` | ❌ minifier bug(下記「症状 C」) |

14/16 ケースで `bestmove G*9g (ponder 9h9g)` に収束。score の
`cp 404` vs `cp 417` は探索深度 20 / 19(5 秒で届く深さ)の違いで、
PV も全部同じ手筋。

### 検証局面 B (2 つ目の局面)

```
lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/L3PP2P/gPR6/1b3G3/3K4L w G2SN7Pgsnp 1
```

結果(`build/eval_results_20260414_090316.jsonl`):

| emscripten | アーキ    | node | browser |
|-----------:|:----------|:--|:--|
| 3.1.43 | x86_64  | ✅ `cp 577 / G*6h` | ✅ `cp 542 / G*6h` |
| 3.1.70 | x86_64  | ❌ `usi timeout` | ✅ `cp 542 / G*6h` |
| 3.1.74 | x86_64  | ❌ `usi timeout` | ✅ `cp 542 / G*6h` |
| 4.0.0  | x86_64  | ❌ `usi timeout` | ✅ `cp 542 / G*6h` |
| 4.0.11 | x86_64  | ✅ `cp 577 / G*6h` | ✅ `cp 542 / G*6h` |
| 4.0.23 | aarch64 | ✅ `cp 577 / G*6h` | ✅ `cp 542 / G*6h` |
| 5.0.0  | aarch64 | ✅ `cp 577 / G*6h` | ✅ `cp 542 / G*6h` |
| 5.0.5  | aarch64 | ✅ `cp 577 / G*6h` | ❌ minifier bug |

局面 A と同様に 14/16 PASS。`bestmove G*6h (ponder 6i6h)` で完全一致。
node 側は cp 577、browser 側は cp 542 と一貫して差が出ているが、これは
探索深度の違い(同じ 5 秒で node は 1 手深く到達)で、両者とも同じ PV
を辿っている。

### 両局面共通の観察

- **Browser 側は 5.0.5 (minifier bug) を除いて全バージョンで動作**。
- **Node 側は 3.1.70 / 3.1.74 / 4.0.0 の 3 バージョンだけが stall**、
  4.0.11 以降は `ccall-only/node` ローダーで問題なく動く。
- **bestmove は全バージョン・両局面で完全一致**(局面 A は `G*9g`、
  局面 B は `G*6h`)。
- **評価値は node と browser で一貫した差**(局面 A は ±13 cp、局面
  B は ±35 cp)があるが、これは両ランナーの探索進行速度の違いで
  bestmove は同じ。つまり、emscripten バージョンによる評価関数の
  ずれは両局面ともに **検出されなかった**。
- 新しい検証局面を追加しても stall の pattern は変わらないので、
  未解決の Node 3 バージョン stall は局面非依存 = 初期化 / pthread
  起動の問題であることが追加でわかった。

### 新しい検証局面の追加方法

runner 側は `--sfen '<sfen>'` を引数で受けるので、単発で試すには:

```bash
bun script/wasm_eval_browser.ts \
  build/3.1.74_x86_64/k-p/web/lib/yaneuraou.k-p.js \
  --think-ms 5000 \
  --sfen 'lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/L3PP2P/gPR6/1b3G3/3K4L w G2SN7Pgsnp 1'
```

`wasm_eval_all.sh` でマトリクス実行するには環境変数 `SFEN` を渡す:

```bash
SFEN='lr5nl/.../w G2SN7Pgsnp 1' THINK_MS=5000 PKG=k-p ./script/wasm_eval_all.sh
```

### Node 側 3 バージョン stall の未解決問題

Node ランナーは **3.1.70 / 3.1.74 / 4.0.0 の 3 バージョンだけ** で
同じ症状 (`usi timeout — tail: []`) で stall する。4.0.11 以降は
まったく同じ loader (`ccall-only/node`) で動いているので、これは
loader 側の問題ではなく emscripten 本体の Node pthread サポートの
問題と推定。

- `[preRun]` と `[onRuntimeInitialized]` は呼ばれる(`__tests__/node_3.1.70_lifecycle.ts` で確認済み)。
- しかし `[postRun]` は呼ばれない — YaneuraOu の `main()` は
  `__EMSCRIPTEN__` 下で REPL ループをスキップして return するので、
  理論的には `postRun` が呼ばれるはず。
- `engine.postMessage("usi")` は wasm_pre.js の queue に積まれるが
  drain されない(postRun 未発火のため)。
- `engine.ccall("usi_command", "number", ["string"], ["usi"])` も
  永続的に `1` (busy) を返す(`__tests__/node_3.1.70_lifecycle.ts`
  で確認)。
- `noInitialRun: true` + 明示 `engine.callMain([])` 経由の init でも
  結果は同じ(`__tests__/node_3.1.70_callmain.ts`)。
- `script/loaders/external_queue.ts` で wasm_pre.js の closure-scoped
  queue を bypass する pump も同じく busy(`__tests__/node_3.1.70_external_queue.ts`)。

**4.0.11 で何が変わったか**: 未調査。emscripten 4.0.11 のチェンジログに
Node pthread 周りの修正が入っている可能性がある。本 repo で動作
しているのは事実として記録しておき、3.1.70-4.0.0 Node の stall
は将来 emscripten を 4.0.11+ 固定にする形で逃げるのが妥当。

### 症状 C (再掲): 5.0.5 Browser の "em-pthread" is not a function

5.0.5 の minified 出力に下記のパターンがある:

```js
m=ba&&globalThis.name=="em-pthread"(function(){function a(){var g=d.shift();…
```

`"em-pthread"(function(){...})` は文字列リテラルを関数として呼び出す式
になり、実行時に `TypeError: "em-pthread" is not a function` で即死する。
上流 emscripten 5.0.5 の minifier バグと思われる。5.0.5 node variant
は別の出力なので影響を受けず `cp 417 / G*9g` で動作している(本節
冒頭のマトリクス参照)。

### 4.0.0 Browser の `cp 417`

4.0.0 の browser だけ `cp 417` を返し、他のバージョンは `cp 404`。
これは単純に、この実行での 5 秒制限内に届いた探索深度が 19 だった
(他は 20)だけで、bestmove `G*9g` は一致しているので regression では
ない。同じ条件で再実行すれば前後に揺れる。

### 次のステップ

- [ ] 全 browser 版で 30 秒探索を回し、3.1.43 baseline (`cp 381 /
      depth 24 / 22.86M nodes`) とバージョン間で揺らぎがないか確認
- [ ] 3.1.70 / 3.1.74 / 4.0.0 node stall を 4.0.11 の emscripten
      チェンジログから辿って原因を特定
- [ ] 5.0.5 の minifier バグの最小再現を作って上流に報告

## 参考ファイル

- Node ランナー: `script/wasm_eval_node.ts`
- Browser ランナー: `script/wasm_eval_browser.ts`
- Browser ランナー HTML: `script/wasm_eval_runner.html`
- 共通 USI フロー: `script/wasm_eval_common.ts`
- Per-generation ローダー: `script/loaders/{types,version,detect,retry,external_queue}.ts` + `script/loaders/{node,browser}/*.ts`
- バッチ: `script/wasm_eval_all.sh` (`RUNNERS=node,browser` で両経路を回す)
- 呼び出しサンプル: `docs/wasm_client_usage.md`
- 再現テスト: `__tests__/*.ts`(特に `node_3.1.70_lifecycle.ts` が Node
  stall の最小再現)
- 計画 / 背景: `docs/wasm_eval_testing_plan.md`
- ビルドログ: `build/multibuild_logs/<ver>_k-p.log`
- 完全 smoke 結果: `build/eval_results_20260414_083156.jsonl`
