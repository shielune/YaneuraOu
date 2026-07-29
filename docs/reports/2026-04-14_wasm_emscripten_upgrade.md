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
| `wasm_eval_results.md` (※現在リポジトリに無い) | TL;DR を「全バージョン dual-runner 動作確認レポート」に書き換え、最終 smoke マトリクスと、残っている Node 3 バージョン stall / 5.0.5 minifier バグの詳細、参考ファイルへのリンクを整理 |

## このセッションのマトリクスは記録しない

> ⚠️ このセッション中に計測した全バージョン動作確認マトリクスは、
> 後日の bisection で **upstream YaneuraOu 本体のバージョンが V8.50
> 以降で評価値が壊れる state** で取得した数値だったと判明したため、
> 記録を削除した。当時観測していた `cp 381` / `cp 404` / `cp 417`
> 帯や bestmove `G*9g` 方向は壊れた state の出力で、現在の正解
> (`B*6f` 方向 / 異なる score 帯) とは一致しない。
>
> 現行の baseline は
> `.claude/skills/wasm-leader/eval_results.md` 冒頭の
> 「2026-04-14 追補: V7.61 downgrade 後の baseline 更新」を参照
> (`a7229610 feat(wasm): downgrade YaneuraOu source to V7.61` 以降)。

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

- `wasm_eval_results.md` — 全バージョンの検証結果と症状の詳細
  (※ 後に `.claude/skills/wasm-leader/` へ統合され、履歴整理でリポジトリからは削除された)
- `docs/wasm_client_usage.md` — 実装時の呼び出し方サンプル
- `wasm_eval_testing_plan.md` — 作業当初の計画と背景 (※同上)
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

---

# 2026-04-14 (後半): SkillLevel 再有効化 + edge variant 追加

作業ブランチ: `feat/enable-skill-level` → `feat/wasm-workers-api`

## セッション全体のゴール

1. 長らくコメントアウトされていた SkillLevel UCI オプションを再度
   有効化し、0-20 の手加減パラメータが動くようにする。
2. V8 Isolate 系ランタイム (Cloudflare Workers / Vercel Edge /
   Deno Deploy) 向けの pthread なし single-thread ビルド (`edge`
   variant) を追加する。
3. edge variant を呼び出すための最小ラッパーをテンプレートとして
   用意し、エンドユーザーがコピペで自分のプロジェクトに組み込める
   ようにする。
4. 棋譜解析用途に備えた複数局面一括評価 API (`evalBatch`) を追加
   する。

## 論理単位ごとの変更一覧

### 1. SkillLevel UCI オプション復活 (`b3e9d7e0`)

**目的**: `source/engine/yaneuraou-engine/yaneuraou-search.cpp` で
「使えていないので削除」の後にコメントアウト状態で残っていた
SkillLevel オプションを再有効化する。

**変更**: yaneuraou-search.cpp の 3 箇所:

- `USI::extra_option` 内で `o["SkillLevel"] << Option(20, 0, 20)` の
  コメントアウトを外す。
- `output_final_pv` lambda と `Thread::search()` で、`Skill skill(
  /*(int)Options["SkillLevel"]*/ 20, 0)` のハードコードを
  `Skill skill((int)Options["SkillLevel"], 0)` に戻す。

**動作確認**: 中盤局面で SkillLevel を 0-20 でスイープすると level 4-7
で 2 番手候補 (`P*8h`) が選ばれるなど、Stockfish の Skill 実装どおり
の確率的 pick_best 挙動が復活したことを確認。

### 2. single-thread `edge` variant の新設 (`e92b712c`)

**目的**: V8 Isolate 系ランタイム (Cloudflare Workers / Vercel Edge
Functions / Cloudflare Pages Functions / Deno Deploy) は `Worker`
コンストラクタと `SharedArrayBuffer` が使えないため、pthread 付きの
web/node 変種はそのままでは動かない。これらを外した single-thread
ビルドを `edge` variant として追加する。既存の web/node 変種は変更
しない。

**変更**:

- `script/wasm_build.js`: `variants` 配列に `edge` エントリ
  (`ENVIRONMENT=web`, `EM_PTHREAD=0`) を追加。さらに `VARIANT` 環境
  変数で build する variant を絞り込めるフィルタを追加
  (`VARIANT=edge node script/wasm_build.js k-p`)。
- `source/Makefile`: `EM_PTHREAD` 変数を新設。`-pthread` と
  `-s PTHREAD_POOL_SIZE=32` を `ifeq ($(EM_PTHREAD),1)` で条件分岐。
- `source/thread.{cpp,h}`: `__EMSCRIPTEN_PTHREADS__` なしビルドでは
  `NativeThread stdThread` メンバを省略し、`start_searching()` /
  `wait_for_search_finished()` で `search()` を呼び出し元スレッドで
  同期実行する経路を追加。`std::thread` コンストラクタが失敗を
  throw して `-fno-exceptions` モードの terminate を引く問題を根本
  から回避。
- `source/wasm_pre.js`: `Module.terminate()` の
  `PThread.terminateAllThreads()` 呼び出しを `typeof` でガード。
  Closure Compiler が `PThread is undeclared` で落ちていた問題を解消。

**成果物**: `build/<ver>_<arch>/k-p/edge/lib/yaneuraou.k-p.{js,wasm}`。
wasm サイズ 1.41 MiB (pthread 付き web/node 変種と比べて -24 KiB)、
Brotli 圧縮 479 KiB。`go movetime 500` の smoke test で
**cold start ~620 ms / warm ~510 ms** を達成。

### 3. edge variant ラッパーテンプレート (`021d7f33`, `7289e8ee`, `5b4b1641`)

**目的**: edge variant をフレームワーク非依存で駆動する最小ラッパー
を `templates/edge/yaneuraou-edge.ts` として提供する。エンドユーザー
がコピペで自分の Workers / Edge プロジェクトに取り込む前提で、
`@types/emscripten` にも依存しない自己完結型にする。

**変更**:

- `templates/edge/yaneuraou-edge.ts`: `createYaneuraOuEdge({ factory,
  wasmBinary, usiHash, hash })` → `engine.eval(req)` /
  `engine.evalBatch(reqs)` API。連続呼び出しは内部 Promise chain で
  自動直列化される。
- `docs/wasm_client_usage.md`: 9 節に edge variant ガイド
  (9.1 制約 / 9.2 ラッパーテンプレート / 9.3 eval vs evalBatch /
  9.4 option の扱い / 9.5 Cloudflare Workers 組み込み例 /
  9.6 実測パフォーマンス) を追加。0 節と 8 節の早見表にも edge 列を
  追加。

**設計ポイント**:

- **Request-level option は毎回 USI 既定値で上書き**
  (`7289e8ee fix(templates): reset request-level options on every eval()`):
  同じ Isolate を複数ユーザーが共有する前提のため、`MultiPV` /
  `SkillLevel` / `DepthLimit` / `NodesLimit` は `eval()` を呼ぶ
  たびに `setoption` で明示的に再設定する。省略時も既定値で上書き
  するので、前のリクエストで立てた値を後続リクエストが引きずらない。
  「user A が `eval({ skillLevel: 5 })` → user B が指定なしで
  `eval()` → user B が user A の設定で思考される」という事故を防ぐ。
- **棋譜解析向けの `evalBatch(reqs)`**
  (`5b4b1641 feat(templates): add evalBatch() for kifu-style continuous analysis`):
  Batch の最初に 1 度だけ `usinewgame` を送ったあと、以降は
  `usinewgame` を挟まずに連続 `position` + `go movetime` を回す。
  Batch 内で置換表が保持されるので、同じ探索時間でも次の局面の
  hash hit 率が上がり実効探索深さが伸びる (smoke test で 2 局面目
  depth 14→15, nodes 140k→188k を確認)。option は配列先頭の要素
  から取り、Batch 内では固定 (途中で option 変更すると TT flush と
  等価になり Batch の旨味が消えるため、意図的に Batch 内 option
  固定としている)。
- **Isolate-level / Request-level の 2 層分離**: `Threads` /
  `USI_Hash` / `Hash` は `createYaneuraOuEdge()` で 1 度だけ設定
  する (置換表の reallocate コストが高いので使い回す)。`MultiPV` /
  `SkillLevel` / `DepthLimit` / `NodesLimit` は `eval()` /
  `evalBatch()` の引数で request ごとに指定する。

## 関連ドキュメント

- `docs/wasm_client_usage.md` — ユーザー向け呼び出しガイド
  (9 節に edge variant 用セクション)
- `.claude/skills/wasm-leader/eval_results.md` — 全バージョンの
  検証結果と症状の詳細 (`docs/` から `.claude/skills/wasm-leader/`
  以下に移動済み)

## このセッションで生まれたコミット

```
773ccda9 docs(wasm): flatten edge variant section structure
5b4b1641 feat(templates): add evalBatch() for kifu-style continuous analysis
7289e8ee fix(templates): reset request-level options on every eval()
021d7f33 docs(wasm): document edge variant and add minimal wrapper template
e92b712c feat(wasm): add single-thread edge variant for V8 Isolate runtimes
b3e9d7e0 feat(engine): re-enable SkillLevel UCI option
```

---

# 2026-04-14 (追補): edge variant を全パッケージに拡張検証

作業ブランチ: `feat/wasm-edge-more-packages`

## ゴール

edge variant を `k-p` 以外の主要パッケージ (`halfkp` / `yaneuraou-mate` /
`tanuki-mate`) でもビルド・実行できるかを確認し、結果をドキュメント
に追記する。ビルドドライバ側のコード変更は不要で、`VARIANT=edge
node script/wasm_build.js <pkg>` で各パッケージが通るかの確認作業。

## 確認結果

| パッケージ | ビルド | smoke | wasm (br 圧縮) | edge 実用性 |
|---|---|---|---|---|
| `k-p` | ✅ | ✅ `go movetime 500` | 1.41 MiB (479 KiB) | **実用可** |
| `halfkp` | ✅ | ✅ `go movetime 500` | **61 MiB** (25 MiB) | Edge bundle size 上限超過。ブラウザ直接配信なら可 |
| `yaneuraou-mate` | ✅ | ⚠️ load + handshake は通るが **`go mate` で `Aborted()`** | 564 KiB (113 KiB) | 現状非対応 (調査保留) |
| `tanuki-mate` | ✅ | ✅ `go mate 2000` → `checkmate nomate` 返却 | 518 KiB (110 KiB) | **実用可** |

### 1. `halfkp` (Suisho5+YaneuraOu NNUE)

- edge variant でのビルドは `thread.{cpp,h}` の single-thread
  経路がそのまま効いて通過。
- 初期局面で `go movetime 500` を実行し、`cp 63 / depth 17 / bestmove
  7g7f` を確認。探索の動作に問題なし。
- **問題は wasm サイズ**: NNUE 重みが埋め込まれて wasm 61 MiB /
  Brotli 25 MiB。Cloudflare Workers Paid プランの bundle size 上限
  10 MiB を大きく超えるため、エッジランタイムには載らない。
- **運用方針**: halfkp を edge 配信したい場合は edge variant を選ばず、
  `web` / `node` variant をブラウザまたは Node サーバから直接使う。
  Edge はあくまでおまけ扱い。

### 2. `yaneuraou-mate` (df-pn 詰将棋ソルバー)

- edge variant でのビルドは通過 (wasm 564 KiB / br 113 KiB)。
- load + `usi` + `isready` までは正常に応答。
- `go mate 2000` を送ると探索開始直後に `Aborted()` で落ちる。
  `source/thread.{cpp,h}` の single-thread stub では捕らえきれない
  pthread / std::thread 依存が `source/mate/` 配下のソルバー実装
  (おそらく df-pn の並列化まわり) に残っている疑いが高い。
- **対応は保留**。Edge はおまけ扱いで、詰将棋用途は後述の
  `tanuki-mate` で代替可能なため、ROI を考えて調査は後回しにする。
  将来必要になった時点で `-sASSERTIONS=1` で再ビルドして原因を
  絞り込む。

### 3. `tanuki-mate` (mate solver)

- edge variant でのビルドは通過 (wasm 518 KiB / br 110 KiB)。
- 簡単な詰み局面 (`4k4/9/4G4/9/9/9/9/9/9 b 2g 1`) に対して
  `go mate 2000` を実行し、`checkmate nomate` を返却 (局面が実際に
  詰みかどうかとは別に、USI 応答が完結することを確認)。
- **Edge で詰将棋探索が欲しい場合のデフォルト選択**。

## コード変更

なし。ビルドドライバ (`script/wasm_build.js`) も Makefile も既存
のままで、`VARIANT` 環境変数と `pkgobjs` 配列の既存項目がそのまま
機能した。変更は `docs/wasm_client_usage.md` 9.2 節の追加のみ。
