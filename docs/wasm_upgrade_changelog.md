# WASM アップグレード作業の変更点まとめ

このドキュメントは **2026-04-13 〜 2026-04-14 のセッション** で、
YaneuraOu の WASM ビルドを emscripten 3.1.43 から 5.0.5 まで動かす
ために加えた変更を、論理単位でまとめたもの。コミット履歴だけだと
関係性が追いづらいため、「何を」「なぜ」「どのファイルで」やったか
を後から読めるようにしておく。

作業ブランチ: `fix/wasm-em-3.1.74`

## 作業全体のゴール

1. emscripten 3.1.43 でしか動かなかった WASM を、3.1.70 / 3.1.74 /
   4.0.x / 5.0.x でも動かす。
2. 同じ `yaneuraou.<pkg>.{js,wasm}` をブラウザと Node の両方から
   呼び出して評価値が一致することを自動検証する。
3. 呼び出し方をアプリ実装側にドキュメント化する。
4. 途中で判明した上流バグ / 保留事項を記録に残して、次回のセッションが
   引き継げるようにする。

## 論理単位ごとの変更一覧

### 1. エージェントチームのスキル定義 (`ea83e7e9`)

**目的**: WASM アップグレード作業を複数の専門ロール(リーダー / コード
修正 / ビルド修正 / ビルド担当 / 実行時評価)に分けて進めるため、各
ロールの責務・入出力・禁止事項を Skill として明文化する。

| 追加したファイル | 責務 |
|---|---|
| `.claude/skills/wasm-leader/SKILL.md` | 全体計画 / 他スキルへの委譲 / 結果の記録 |
| `.claude/skills/wasm-code-fixer/SKILL.md` | `source/*.cpp`, `source/wasm_pre.js`, 再現テストなどソース修正 |
| `.claude/skills/wasm-build-config/SKILL.md` | `source/Makefile` の em++ ブランチ(LDFLAGS, INCOMING_MODULE_JS_API 等)の調整 |
| `.claude/skills/wasm-builder/SKILL.md` | `script/wasm_build.js` / docker 経由の実ビルド |
| `.claude/skills/wasm-eval-runner/SKILL.md` | Node + Playwright の dual runner と結果集約 |

- `.gitignore` の `.*/` を回避するために `!.claude/` の例外を追加
  (`.claude/settings.local.json` だけは個人設定として除外)。
- その後のセッションで要件が増えたぶんも各スキルに反映済み(後述の
  Node 向けビルド追加や dual runner は `wasm-eval-runner` と
  `wasm-build-config` に反映)。

### 2. devcontainer への Playwright / Bun / Node 導入 (`24a6dd5b`)

**目的**: ブラウザ経路の検証を headless Chromium で回すため、devcontainer
内で Playwright を立ち上げられるようにする。

| 変更したファイル | 内容 |
|---|---|
| `.devcontainer/Dockerfile` | Chromium が必要とする `libnss3` / `libatk*` / `libdrm2` / 各種フォント (`fonts-noto-color-emoji`, `fonts-ipafont-gothic` 等) を apt-get |
| `.devcontainer/devcontainer.json` | `ghcr.io/shyim/devcontainers-features/bun:0` と `ghcr.io/devcontainers/features/node:1` を追加 |
| `.devcontainer/postCreateCommand.sh` | `bun install --frozen-lockfile --ignore-scripts` と `bunx playwright install-deps chromium` を走らせる |

- `.gitignore` に `!.devcontainer/` の例外も併せて追加(既存の `.*/` が
  devcontainer のトラッキングを阻んでいたため)。

### 3. ブラウザ経路の復旧 (INCOMING_MODULE_JS_API + 静的サーバ prelude)

**目的**: emscripten 3.1.74 で `INCOMING_MODULE_JS_API` のデフォルトから
`print`/`printErr`/`postRun`/`preRun` が外れた結果、`wasm_pre.js` の
`Module.print` 上書きが Closure minifier でデッドコード除去される症状を
直す。

| 変更したファイル | 内容 | コミット |
|---|---|---|
| `source/Makefile` | em++ ブランチに `-s INCOMING_MODULE_JS_API=print,printErr,postRun,preRun,…` を明示 | `813bc30b`(セッション前) |
| `script/wasm_eval_browser.ts` | 静的サーバが engine JS に pthread prelude(worker 内の `console.log` を `postMessage({__yaneurao_stdout, text})` に差し替える)を注入 | `624d4ff4` 以降 |
| `script/loaders/browser/common.ts` | main スレッド側で `installConsoleTap` と `installWorkerStdoutTap` を実装(前者は `console.log` を、後者は `window.Worker` を patch して worker stdout を回収) | `624d4ff4` |

結果として、Browser 経路は 3.1.74 / 4.0.x / 5.0.0 まで復旧した。
5.0.5 だけは emscripten 側の minifier バグ (`"em-pthread"(function(){…})`
が文字列を関数呼び出しする構文) で別問題として残っている。

### 4. 共有 loader フレームワーク (`624d4ff4`)

**目的**: emscripten の世代差(classic worker / ES-module worker /
ccall-only)を 1 つの `EngineInstance` インターフェースの裏側に隠して、
runner とアプリ実装がバージョンに依存しないようにする。

追加したファイル(`script/loaders/` 配下):

| ファイル | 役割 |
|---|---|
| `types.ts` | `EngineInstance` / `LoaderContext` / `Loader` インターフェース |
| `version.ts` | バージョン比較ヘルパ + `classifyVersion` で 3 世代に分類 |
| `detect.ts` | ビルドパス → `LoaderContext` 変換と loader 選択 |
| `node/common.ts` | Node 環境の polyfill (`ensureWebGlobals`, `installWorkerPolyfill`, `installMainConsoleTap`, `instantiateWithUnifiedStdout`) |
| `node/worker_shim.ts` | `worker_threads.Worker` の中で走るシム(pthread 判定用 globals + classic worker eval + ES module import) |
| `node/classic_worker.ts` | 3.1.43 用 loader |
| `node/esmodule_worker.ts` | 3.1.44 ≤ v < 3.1.74 用 loader |
| `node/ccall_only.ts` | v ≥ 3.1.74 用 loader |
| `node/index.ts` | 上記 3 つの loader を優先順付きで export |
| `browser/common.ts` | ブラウザ環境の `loadEngine` + console/Worker tap |
| `browser/{classic_worker,esmodule_worker,ccall_only}.ts` | ブラウザ側の世代別 loader |
| `browser/index.ts` | 同上 |
| `retry.ts` | `ccallWithRetry` ヘルパ(ccall が `1`=busy を返したときの backoff retry) |
| `external_queue.ts` | `wasm_pre.js` の closure-scoped queue を bypass して engine.ccall を直接駆動する pump(`9a003dd3`) |

すべての loader は最終的に `installExternalQueue(engine)` で
`engine.postMessage` を自前の pump に差し替え、`sendCommand` は
ccall に集約する設計に統一されている(wasm_pre.js の `postRun`
依存を避けるため)。

### 5. dual runner (`624d4ff4` ほか)

**目的**: 同じ artefact を Node(`node:worker_threads`)と
ブラウザ(Playwright + Chromium)の両方から駆動できるようにし、
片方だけ PASS / 片方だけ FAIL になったときにそれ自体が診断の
シグナルとして使えるようにする。

| 追加・変更したファイル | 内容 |
|---|---|
| `script/wasm_eval_node.ts`(新規) | bun で実行される Node 側 entry。`detect.ts` + `nodeLoaders` + `runUsiEval` を直列でつなぐ |
| `script/wasm_eval_common.ts`(新規) | 環境非依存の USI フロー (`usi → setoption → isready → position → go`) |
| `script/wasm_eval_browser.ts`(大幅リファクタ) | `Bun.Transpiler` で `.ts` をオンザフライ transpile して static server から配信、runner.html は `/loaders/*.ts` と `/script/wasm_eval_common.ts` を dynamic import |
| `script/wasm_eval_runner.html`(リファクタ) | ccall のインライン実装を捨て、`browserLoaders` + `runUsiEval` を使うように |
| `script/wasm_eval_all.sh`(改修) | `RUNNERS=node,browser` のリストに従い、`*/node/lib/*` と `*/web/lib/*` を別々にイテレートして各 runner に渡す |

### 6. Node / Browser 別ビルド (`978c4aa0`, `a9278311`)

**目的**: 当初は `ENVIRONMENT=web,worker,node` の 1 変種ビルドで両経路を
狙っていたが、Node と Browser では必要な `EXPORTED_RUNTIME_METHODS` と
`ENVIRONMENT` が異なるため、同じソースから 2 種類の artefact を吐く
方針に変更。

| 変更したファイル | 内容 |
|---|---|
| `source/Makefile` | em++ ブランチで `EM_ENVIRONMENT` と `EM_EXPORTED_RUNTIME_METHODS` を `?=` で受け取れるように変数化。デフォルトは web 版 |
| `script/wasm_build.js` | `pkgobj` ループの内側に `variants` ループを追加。`web` 変種は `ENVIRONMENT=web,worker` + `EXPORTED_RUNTIME_METHODS=['FS','ccall']`、`node` 変種は `ENVIRONMENT=node` + `EXPORTED_RUNTIME_METHODS=['FS','ccall','callMain']`。出力先を `build/<ver>_<arch>/<pkg>/{web,node}/lib/` に分岐 |
| `script/wasm_build.js` | `.worker.js` を optional 扱いに変更(3.1.60+ では worker が main JS にインライン化されるため)。また `stdio:"inherit"` を削除して `child.stdout`/`child.stderr` を本当の pipe stream にし、ビルドログが stdout に流れるようにした |
| `script/loaders/detect.ts` | 新旧両レイアウトに対応するため、パス parsing を `parts[length-5]` まで探索するように拡張。`detectVariantFromJsPath` で `web`/`node` を返すヘルパも追加 |
| `script/wasm_eval_{node,browser}.ts` | `versionTag` / `engineTag` の計算を新レイアウト(1 段深くなった分)に合わせる |
| `script/wasm_eval_all.sh` | `find build -path '*/<variant>/lib/yaneuraou.*.js'` で variant 別に対象を収集するように |

### 7. Node の callMain 経路 (`c15edafb`, `a9278311`)

**目的**: Node 側で `main()` を emscripten が自動起動すると、特定の
バージョン(3.1.70 / 3.1.74 / 4.0.0)で engine が busy ロック状態に
陥ることが分かったため、Node 変種だけ `noInitialRun: true` + 明示
`engine.callMain([])` 経路にした。

| 変更したファイル | 内容 |
|---|---|
| `source/Makefile` | Node 変種の `EXPORTED_RUNTIME_METHODS` に `callMain` を追加 |
| `script/loaders/node/common.ts` | `instantiateEngine` に `noInitialRun: true` を追加、`runMainInit(engine)` で `engine.callMain([])` を呼ぶ。emscripten が `ExitStatus` を `throw` する normal exit 経路を swallow |

結果として 4.0.11 以降は動作するようになったが、3.1.70 / 3.1.74 /
4.0.0 の 3 バージョンは callMain を呼んでも同じ symptoms で stall
する(pthread layer の問題と推定)。

### 8. 呼び出しサンプル文書化 (`c15edafb`, `c5defade`)

**目的**: アプリ実装者が 3.1.43 の正攻法(`postMessage` +
`addMessageListener`)と 3.1.74+ の `ccall` 経路、Node の
`worker_threads` 経由、それぞれをコピペで試せるようにする。

| 追加したファイル | 内容 |
|---|---|
| `docs/wasm_client_usage.md` | 3.1.43 / 3.1.44-3.1.73 / 3.1.74+ / Node の 4 系統の最小コード、必要な Makefile フラグ、COOP/COEP ヘッダ、loader アーキテクチャの使い方、ビルドフラグ早見表 |

### 9. 再現テスト (`87405a54`)

**目的**: デバッグ中に書いた ad-hoc script (`/tmp/test_*.mjs`) を
そのまま捨てるのではなく、`__tests__/*.ts` に移して次回以降の調査の
土台にする。

| 追加したファイル | 内容 |
|---|---|
| `__tests__/README.md` | 各ファイルの目的 |
| `__tests__/node_3.1.43_smoke.ts` | 3.1.43 node 変種の最小 smoke |
| `__tests__/node_3.1.70_lifecycle.ts` | preRun / onRuntimeInitialized / postRun / print / addMessageListener の発火タイミング trace |
| `__tests__/node_3.1.70_noinit_ccall.ts` | `noInitialRun:true` + 直接 ccall の挙動確認 |
| `__tests__/node_3.1.70_callmain.ts` | `noInitialRun:true` + 明示 `callMain` |
| `__tests__/node_3.1.70_external_queue.ts` | 外部 queue pump 経由の挙動 |

assertion test ではなく、`bun __tests__/<name>.ts` で逐次 stderr に
出力する reproducer。期待観察値は各ファイル先頭のコメントに記述。

### 10. 結果ドキュメントの集約 (`0116d0ed`, `0ac1c11b`, `33e468b8`)

**目的**: 検証結果を 1 か所にまとめ、バージョンマトリクスで全体像を
掴めるようにする。

| 変更したファイル | 内容 |
|---|---|
| `docs/wasm_eval_results.md` | TL;DR を「全バージョン dual-runner 動作確認レポート」に書き換え、最終 smoke マトリクスと、残っている Node 3 バージョン stall / 5.0.5 minifier バグの詳細、参考ファイルへのリンクを整理 |

## 最終的な動作確認マトリクス (k-p, --think-ms 5000)

| emscripten | node  | browser |
|---|---|---|
| 3.1.43 | ✅ cp 417 / G*9g | ✅ cp 404 / G*9g |
| 3.1.70 | ❌ usi timeout | ✅ cp 404 / G*9g |
| 3.1.74 | ❌ usi timeout | ✅ cp 404 / G*9g |
| 4.0.0  | ❌ usi timeout | ✅ cp 417 / G*9g |
| 4.0.11 | ✅ cp 417 / G*9g | ✅ cp 404 / G*9g |
| 4.0.23 | ✅ cp 417 / G*9g | ✅ cp 404 / G*9g |
| 5.0.0  | ✅ cp 417 / G*9g | ✅ cp 404 / G*9g |
| 5.0.5  | ✅ cp 417 / G*9g | ❌ minifier bug |

14 / 16 PASS、bestmove は全ケースで `G*9g` 一致。

## 残課題

- 3.1.70 / 3.1.74 / 4.0.0 node stall の根本原因特定。4.0.11 の
  emscripten changelog を辿って pthread / Node runtime 周りの変更を
  探す。
- 5.0.5 browser の `"em-pthread" is not a function` minifier バグの
  最小再現を作って上流に報告。
- 全 browser 版で 30 秒探索を回し、3.1.43 baseline (`cp 381 /
  depth 24 / 22.86M nodes`) と揺らぎなく一致するか確認(現時点では
  5 秒探索で bestmove 一致まで)。

## 関連ドキュメント

- `docs/wasm_eval_results.md` — 全バージョンの検証結果と症状の詳細
- `docs/wasm_client_usage.md` — 実装時の呼び出し方サンプル
- `docs/wasm_eval_testing_plan.md` — 作業当初の計画と背景
- `__tests__/README.md` — 再現テストの目録

## このセッションで生まれたコミット

```
33e468b8 docs(wasm): dual-runner smoke matrix across all 8 emscripten versions
87405a54 test(wasm): add reproduction tests for node loader lifecycle issues
a9278311 fix(wasm): 2-variant builder resilience and clearer logging
c5defade docs(wasm): update usage guide for the 2-variant build layout
978c4aa0 feat(wasm): split wasm_build into web and node variants
c15edafb docs(wasm): add per-version WASM client usage guide
9a003dd3 fix(wasm): drive sendCommand through an external ccall queue
0ac1c11b docs(wasm): record dual-runner smoke test results for 3.1.43 and 3.1.70
6f8a717b fix(wasm): harden dual-runner loaders with NodeWorker split and ccall retry
624d4ff4 feat(wasm): dual Node+Playwright runners with per-generation loaders
0116d0ed docs(wasm): refocus eval results around the 3.1.74+ runtime break
24a6dd5b build(devcontainer): add Playwright runtime deps and Bun/Node features
ea83e7e9 feat(wasm): add Claude Code agent team skills for upgrade workflow
```

(`813bc30b` 以前のコミットはこのセッション前の準備作業。)
