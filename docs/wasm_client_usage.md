# WASM クライアント実装ガイド

`source/Makefile` と `script/wasm_build.js` が生成する
`yaneuraou.<pkg>.{js,wasm}` をフロントエンド/バックエンドから呼び出す
コードサンプル集。**バージョンごとの違い** を含めて、実装時にそのまま
コピペできるコードを並べる。

本書の対象:
- 自分のアプリに YaneuraOu (WASM) を組み込む人
- `script/wasm_eval_*` を自作ランナーのベースにしたい人

扱うバージョン:

| emscripten | ブラウザ実装 | Node 実装 | メモ |
|---|---|---|---|
| 3.1.43 | ✅ `postMessage` 方式 | ✅ `postMessage` 方式 + worker_threads shim | classic worker (`.worker.js`) |
| 3.1.44 – 3.1.73 | ✅ `postMessage` 方式 | ⚠ 3.1.60+ は stall(未解決) | ES module worker |
| 3.1.74+ | ✅ `postMessage` 方式 | ⚠ 未解決 | `INCOMING_MODULE_JS_API` 明示が必要 |

「未解決」の Node 側は 2026-04-14 現在の調査状況。原因仮説は
`docs/wasm_eval_results.md` の「Node 側 3.1.60+ 共通の未解決問題」節を
参照。

---

## 0. 前提: ビルド成果物のレイアウト

`make build` 経由で `script/wasm_build.js` を呼ぶと、パッケージ 1 つに
つき **web 変種・node 変種・edge 変種の 3 つ** が生成される。同じソースから
`EM_ENVIRONMENT` / `EM_EXPORTED_RUNTIME_METHODS` / `EM_PTHREAD` だけ
切り替えて別々にビルドされる (`script/wasm_build.js` の `variants` 配列
参照):

```
build/<emscripten-ver>_<arch>/k-p/
├── web/                       ← ブラウザ向け (pthread + web worker)
│   └── lib/
│       ├── yaneuraou.k-p.js          ← ES module factory (import 対象)
│       ├── yaneuraou.k-p.wasm        ← 本体
│       ├── yaneuraou.k-p.worker.js   ← ★3.1.43 のみ (classic worker)
│       ├── yaneuraou.k-p.{js,wasm}.{br,gz}   ← 配信用圧縮済み
│       ├── yaneuraou.k-p.d.ts
│       └── yaneuraou.module.d.ts
├── node/                      ← Node 向け (pthread + worker_threads)
│   └── lib/
│       └── (同じ構成)
└── edge/                      ← V8 Isolate 系ランタイム向け (single-thread)
    └── lib/
        └── (同じ構成、`yaneuraou.k-p.worker.js` は空)
```

**web 変種** のビルドフラグ:

```
-s ENVIRONMENT=web,worker
-s "EXPORTED_RUNTIME_METHODS=['FS','ccall']"
-pthread -s PTHREAD_POOL_SIZE=32
```

**node 変種** のビルドフラグ:

```
-s ENVIRONMENT=node
-s "EXPORTED_RUNTIME_METHODS=['FS','ccall','callMain']"
-pthread -s PTHREAD_POOL_SIZE=32
```

**edge 変種** のビルドフラグ:

```
-s ENVIRONMENT=web
-s "EXPORTED_RUNTIME_METHODS=['FS','ccall']"
(pthread / PTHREAD_POOL_SIZE なし)
```

3 変種共通:

```
--pre-js wasm_pre.js
-s EXPORT_ES6=1 -s MODULARIZE=1
-s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=<pkg> -s MAXIMUM_MEMORY=4294967296
-s STACK_SIZE=67108864
-s INCOMING_MODULE_JS_API=print,printErr,postRun,preRun,...    # 3.1.74+ 必須
```

特定の variant だけをビルドしたい場合は `VARIANT` 環境変数で絞り込める:

```bash
VARIANT=edge node script/wasm_build.js k-p
```

`yaneuraou.k-p.js` はすべてのバージョンで **ES module** (`-s
EXPORT_ES6=1 -s MODULARIZE=1`)。default export が
`EmscriptenModuleFactory<YaneuraOuModule>`。

### Module 型(全バージョン共通)

`yaneuraou.module.d.ts` がこれを吐く:

```ts
/// <reference types="emscripten" />

export interface YaneuraOuModule extends EmscriptenModule {
  addMessageListener: (listener: (line: string) => void) => void
  removeMessageListener: (listener: (line: string) => void) => void
  postMessage: (command: string) => void
  terminate: () => void
  ccall: typeof ccall
  FS: typeof FS
}
```

`addMessageListener` / `postMessage` / `terminate` は
`source/wasm_pre.js` の `--pre-js` が埋め込んだもの。`ccall` / `FS` は
`EXPORTED_RUNTIME_METHODS` で露出されている。

---

## 1. ブラウザ — 3.1.43 (classic worker)

`engine/index.ts` が既に実装している正攻法。`postMessage` で駆動、
`addMessageListener` で応答を受ける。import するのは **web 変種** の
artefact(`build/<ver>_<arch>/k-p/web/lib/yaneuraou.k-p.js`)。

```ts
import YaneuraOu_K_P from './lib/yaneuraou.k-p'
import type { YaneuraOuModule } from './lib/yaneuraou.module'

// factory を await すると runtime が ready になった YaneuraOuModule が返る
const engine: YaneuraOuModule = await YaneuraOu_K_P()

// 応答はすべて addMessageListener 経由
engine.addMessageListener((line) => {
  if (line === 'usiok' || line === 'readyok') { /* ... */ }
  if (line.startsWith('bestmove')) { /* ... */ }
  if (line.startsWith('info')) { /* ... */ }
})

// USI コマンドは postMessage で送る
engine.postMessage('usi')
// → 'id name ...' 'option name ...' 'usiok' が listener に届く
engine.postMessage('setoption name Threads value 1')
engine.postMessage('setoption name USI_Hash value 64')
engine.postMessage('isready')
// → 'readyok'
engine.postMessage('position sfen lr5nl/2P2+S1k1/...')
engine.postMessage('go btime 0 wtime 0 byoyomi 5000')
// → 'info ...' 連打 → 'bestmove ...'

// 終了
engine.terminate()
```

**前提となるビルドフラグ (`source/Makefile` の em++ ブランチで設定済み):**

```make
LDFLAGS += --pre-js wasm_pre.js           # USI queue/postMessage を注入
LDFLAGS += -s MODULARIZE=1 -s EXPORT_ES6=1
LDFLAGS += -s ENVIRONMENT=web,worker       # browser 版
LDFLAGS += -s "EXPORTED_RUNTIME_METHODS=['FS','ccall']"
LDFLAGS += -s PTHREAD_POOL_SIZE=32
LDFLAGS += -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=<pkg> -s MAXIMUM_MEMORY=4294967296
```

**サーバー側の必須ヘッダ** — `crossOriginIsolated` → `SharedArrayBuffer`
→ pthread を使うので以下の 2 本が必要:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Vite/Next/Astro など dev サーバで入れ忘れると `not crossOriginIsolated`
で即死する。

### 3.1.43 固有の注意点

- `yaneuraou.k-p.worker.js` は **別ファイル** として出力される(classic
  script)。配信時に `yaneuraou.k-p.js` と同じディレクトリに置くこと。
- minify が若干弱く生成 JS サイズも大きめ。

---

## 2. ブラウザ — 3.1.44 〜 3.1.73 (ES module worker)

**コードは 3.1.43 とまったく同じ**。変わるのはビルド側だけ:

- `yaneuraou.k-p.worker.js` は生成されなくなる(pthread worker は main JS
  にインライン統合される)
- 配信時は `yaneuraou.k-p.js` と `.wasm` の 2 本だけでよい

```ts
import YaneuraOu_K_P from './lib/yaneuraou.k-p'

const engine = await YaneuraOu_K_P()
engine.addMessageListener((line) => { /* ... */ })
engine.postMessage('usi')
// ... 以下 3.1.43 と同じ
```

Makefile 側も同じで OK。

---

## 3. ブラウザ — 3.1.74+ (INCOMING_MODULE_JS_API 明示必須)

アプリ側のコードは **依然として 3.1.43 と同じ**。ただし Makefile に
以下の行が **必須**:

```make
# 3.1.74 以降、INCOMING_MODULE_JS_API のデフォルトから
# print / printErr / postRun / preRun が外れた。Closure が
# Module.print 経路をデッドコード除去するので、wasm_pre.js の
# postMessage / addMessageListener が効かなくなる。
LDFLAGS += -s INCOMING_MODULE_JS_API=print,printErr,postRun,preRun,\
           onAbort,onExit,onRuntimeInitialized,wasmBinary,locateFile,\
           instantiateWasm,mainScriptUrlOrBlob,noInitialRun,\
           noExitRuntime,arguments
```

これが無いと、ブラウザで `engine.postMessage('go ...')` を呼んでも `info`
/ `bestmove` が **一切返ってこない**(`docs/wasm_eval_results.md` の
「症状 B」)。

### 3.1.74+ で `ccall` ベースの呼び出しも可能

wasm_pre.js の queue/poll に依存したくない場合は、`engine.ccall` で
直接 USI コマンドを投げる方式も使える。
`script/loaders/browser/ccall_only.ts` が最小実装。

```ts
import YaneuraOu_K_P from './lib/yaneuraou.k-p'

const engine = await YaneuraOu_K_P({
  print: (line) => onLine(line),
  printErr: (line) => onLine('[err] ' + line),
})

// ccall("usi_command", ...) は 1 ("busy, retry") を返すことがあるので
// backoff 付きで呼び出す
async function sendCommand(cmd: string) {
  const deadline = Date.now() + 5000
  let backoff = 1
  while (Date.now() < deadline) {
    const tryLater = engine.ccall('usi_command', 'number', ['string'], [cmd])
    if (!tryLater) return
    await new Promise((r) => setTimeout(r, backoff))
    backoff = Math.min(backoff * 2, 200)
  }
  throw new Error(`sendCommand busy timeout: ${cmd}`)
}

await sendCommand('usi')
await sendCommand('setoption name Threads value 1')
await sendCommand('isready')
await sendCommand('position sfen ...')
await sendCommand('go btime 0 wtime 0 byoyomi 5000')
```

pthread-worker 側の stdout が main thread に届かないケース
(`docs/wasm_eval_results.md` の症状 B)に対処するため、以下のパッチを
併用する必要がある:

```ts
// 1. main thread の console.log を tap — 3.1.74+ の minified main JS は
//    Module.print を無視して console.log を直接叩くため
const origLog = console.log
console.log = (...args) => onLine(args.map(String).join(' '))

// 2. 各 pthread worker からの stdout を postMessage 経由で拾う
const RealWorker = window.Worker
class PatchedWorker extends RealWorker {
  constructor(url, opts) {
    super(url, opts)
    super.addEventListener('message', (ev) => {
      if (ev?.data?.__yaneurao_stdout) onLine(String(ev.data.text))
    })
  }
}
window.Worker = PatchedWorker

// 3. Worker 内の console.log を postMessage に差し替えるプレリュード
//    (これはサーバ側で engine の .js の先頭に prepend する必要がある)
//    script/wasm_eval_browser.ts の PTHREAD_STDOUT_PRELUDE を参照
```

この 3 点セットは `script/loaders/browser/common.ts` の
`installConsoleTap` + `installWorkerStdoutTap`、`loadEngineWithUnifiedStdout`
が実装している。新規アプリではそちらをコピーするか、同等の 3 点セットを
手で入れる。

---

## 4. Node — 3.1.43 (worker_threads 経由)

Node から YaneuraOu を呼ぶときは **node 変種**
(`build/<ver>_<arch>/k-p/node/lib/yaneuraou.k-p.js`) を使う。
`ENVIRONMENT=node` でビルドされている分 Node 固有の runtime 分岐が
生きていて、`engine.callMain` も露出している。

```ts
import { Worker as NodeWorker } from 'node:worker_threads'
import fs from 'node:fs'
import path from 'node:path'
import url from 'node:url'

// 1. globalThis に web 環境用の globals を生やす
globalThis.self ??= globalThis
globalThis.window ??= globalThis
globalThis.location ??= { href: url.pathToFileURL(jsPath).href }
globalThis.document ??= {
  currentScript: { src: url.pathToFileURL(jsPath).href },
  createElement: () => ({}),
}

// 2. fetch を file:// に対応させる (Node の undici は file:// を拒否する)
const origFetch = globalThis.fetch
globalThis.fetch = async (input, init) => {
  const s = typeof input === 'string' ? input : String(input?.url ?? input)
  if (s.startsWith('file://')) {
    return new Response(fs.readFileSync(url.fileURLToPath(s)))
  }
  return origFetch(input, init)
}

// 3. Worker ポリフィル — emscripten の `new Worker(URL, opts)` を
//    NodeWorker にブリッジする。3.1.43 では worker が ".worker.js" の
//    classic script なので、shim で eval する経路も必要
//    (script/loaders/node/worker_shim.ts 参照)
globalThis.Worker = class {
  constructor(target, opts) {
    const file = target instanceof URL
      ? url.fileURLToPath(target)
      : path.resolve(String(target))
    this._w = new NodeWorker(shimUrl, { workerData: { workerFile: file } })
    this._w.on('message', (m) => this.onmessage?.({ data: m }))
  }
  postMessage(msg) { this._w.postMessage(msg) }
  terminate() { return this._w.terminate() }
  addEventListener(type, fn) {
    if (type === 'message') this.onmessage = (e) => fn(e)
  }
  removeEventListener() {}
}

// 4. engine を import して呼ぶ
const libDir = path.dirname(jsPath)
// lib/ 内に ESM 宣言が必要
if (!fs.existsSync(path.join(libDir, 'package.json'))) {
  fs.writeFileSync(path.join(libDir, 'package.json'), '{"type":"module"}\n')
}
const mod = await import(url.pathToFileURL(jsPath).href)
const factory = mod.default ?? mod
const wasmBinary = fs.readFileSync(jsPath.replace(/\.js$/, '.wasm'))

const engine = await factory({
  wasmBinary,
  mainScriptUrlOrBlob: url.pathToFileURL(jsPath).href,
  locateFile: (p) => path.join(libDir, p),
  instantiateWasm: (imports, receiveInstance) => {
    WebAssembly.instantiate(wasmBinary, imports).then(({ instance, module }) =>
      receiveInstance(instance, module))
    return {}
  },
})

// 5. 以降はブラウザ版と同じ
engine.addMessageListener((line) => { /* ... */ })
engine.postMessage('usi')
```

### worker_shim の最小実装

上の `shimUrl` が指すファイル — worker_threads.Worker の中で走らせる:

```ts
// worker_shim.ts (worker_threads 内で実行される)
import { parentPort, workerData } from 'node:worker_threads'
import fs from 'node:fs'
import url from 'node:url'

// em-pthread として emscripten に認識させる
globalThis.self = globalThis
Object.defineProperty(globalThis, 'name', {
  value: 'em-pthread', writable: true, configurable: true,
})
globalThis.importScripts = () => {}
globalThis.location = { href: url.pathToFileURL(workerData.workerFile).href }
globalThis.postMessage = (msg) => parentPort.postMessage(msg)

let onmessage = null
parentPort.on('message', (m) => onmessage?.({ data: m }))
Object.defineProperty(globalThis, 'onmessage', {
  get() { return onmessage }, set(fn) { onmessage = fn }, configurable: true,
})

// fetch(file://) 対応
const origFetch = globalThis.fetch
globalThis.fetch = async (input) => {
  const s = typeof input === 'string' ? input : String(input?.url ?? input)
  if (s.startsWith('file://')) {
    return new Response(fs.readFileSync(url.fileURLToPath(s)))
  }
  return origFetch(input)
}

const workerFile = workerData.workerFile
if (workerFile.endsWith('.worker.js')) {
  // classic script: eval into global scope
  ;(0, eval)(fs.readFileSync(workerFile, 'utf8'))
} else {
  // ES module: dynamic import
  await import(url.pathToFileURL(workerFile).href)
}
```

完全版は `script/loaders/node/worker_shim.ts` を参照。

---

## 5. Node — 3.1.60+ (現状未解決)

**2026-04-14 現在、Node で 3.1.60+ ビルドを動作させる決定打は見つかって
いない。** 現象と調査ログ:

- `preRun` と `onRuntimeInitialized` は発火する
- main() は `USI::init(Options)` / `Search::init()` / `Threads.set(1)` /
  `Eval::init()` を実行して return する (`source/main.cpp` の
  `#if !defined(__EMSCRIPTEN__)` で `USI::loop` がスキップされるため)
- それにもかかわらず `engine.ccall("usi_command", "number", ["string"],
  ["usi"])` が **永久に 1 (busy) を返す**
- `engine.postMessage("usi")` も wasm_pre.js の queue に入ったまま
  drain されない

`EXPORTED_RUNTIME_METHODS` に `callMain` を追加して
`noInitialRun: true` + 明示 `engine.callMain([])` で main だけ手動実行
する経路も試したが、結果は同じ(usi timeout / tail=[])。

**暫定策 — ブラウザ経路を使う**: Node から評価値を取るなら、現状は
headless Chromium (Playwright) 経由の
`script/wasm_eval_browser.ts` を呼び出すのが最短。実働している。

```bash
bun script/wasm_eval_browser.ts \
  build/3.1.74_x86_64/k-p/lib/yaneuraou.k-p.js \
  --think-ms 5000
# → JSON で score / bestmove / lastInfo を stdout に出す
```

調査継続中。解決したら本節を書き換える。

---

## 6. 既存のランナースクリプトの使い方

検証/バッチ用のランナーは `script/` 以下にある。新規実装する前に
こっちで十分ならそれを使うのが手早い。

### シングル実行 (browser)

```bash
bun script/wasm_eval_browser.ts \
  build/<ver>_<arch>/<pkg>/web/lib/yaneuraou.<pkg>.js \
  --think-ms 5000
```

出力は 1 塊の JSON (`runner`, `version`, `engine`, `score`, `bestmove`,
`lastInfo`, `infoCount`)。

### シングル実行 (node)

```bash
bun script/wasm_eval_node.ts \
  build/3.1.43_x86_64/k-p/node/lib/yaneuraou.k-p.js \
  --think-ms 5000
```

3.1.43 のみ動作確認済み。3.1.60+ は現状 stall(調査中)。

### 全バージョン/両ランナー一括実行

```bash
THINK_MS=5000 PKG=k-p RUNNERS=node,browser ./script/wasm_eval_all.sh
# 結果は build/eval_results_<timestamp>.jsonl に追記
```

`RUNNERS=browser` で Playwright 片方だけ、`RUNNERS=node` で Node 片方
だけも可。

### Per-generation ローダーを直接使う

`script/loaders/` は「一つの `EngineInstance` インターフェースで全世代を
抽象する」ためのもの。新規アプリでも同じ構造を流用できる。

```ts
import { buildContext, pickLoader } from './script/loaders/detect'
import { nodeLoaders } from './script/loaders/node'
// or: import { browserLoaders } from './script/loaders/browser'
import { runUsiEval } from './script/wasm_eval_common'

const ctx = buildContext('node', jsPath)
// versionOverride を渡してパスベースの自動検出を迂回することも可能
// const ctx = buildContext('browser', '/engine/yaneuraou.k-p.js', '3.1.74')

const loader = pickLoader(nodeLoaders, ctx.emscriptenVersion)
const engine = await loader.load(ctx)

engine.onLine((line) => console.log(line))
const result = await runUsiEval(engine, {
  sfen: 'lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1',
  thinkMs: 5000,
  threads: 1,
  hash: 64,
})
console.log(result)
await engine.dispose()
```

---

## 7. バージョンをまたいでコードを共有したいとき

推奨: `script/loaders/types.ts` の `EngineInstance` インターフェースを
そのままコピー、または import して、アプリケーション側は以下の 3 点
だけに依存する:

```ts
interface EngineInstance {
  sendCommand(cmd: string): Promise<void>
  onLine(listener: (line: string) => void): void
  dispose(): Promise<void>
}
```

裏で `postMessage` を使うか `ccall` を使うか、classic worker か ES
module worker かは loader 側の責任。アプリは USI コマンドのシーケンス
(`usi` → `setoption` → `isready` → `position` → `go` → `bestmove` 待ち)
だけに集中できる。

---

## 8. ビルドフラグ早見表

| 項目 | web 変種 / 3.1.43 | web / 3.1.60–3.1.73 | web / 3.1.74+ | node 変種 | edge 変種 |
|---|---|---|---|---|---|
| ビルド出力先 | `build/.../k-p/web/lib/` | 同 | 同 | `build/.../k-p/node/lib/` | `build/.../k-p/edge/lib/` |
| `ENVIRONMENT` | `web,worker` | `web,worker` | `web,worker` | `node` | `web` |
| `EXPORT_ES6` / `MODULARIZE` | 必須 | 必須 | 必須 | 必須 | 必須 |
| `EXPORTED_RUNTIME_METHODS` | `['FS','ccall']` | `['FS','ccall']` | `['FS','ccall']` | `['FS','ccall','callMain']` | `['FS','ccall']` |
| `INCOMING_MODULE_JS_API` 明示 | 不要 | 不要 | **必須** | **必須** | **必須** |
| `-pthread` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `PTHREAD_POOL_SIZE` | 32 | 32 | 32 | 32 | — |
| `--pre-js wasm_pre.js` | 必須 | 必須 | 必須 | 必須 | 必須 |
| 別 `.worker.js` が出る | ✅ | ❌ | ❌ | ❌ | ❌ |
| サーバ側 COOP/COEP | 必須 | 必須 | 必須 | N/A | 不要 |
| アプリ側 `console.log` tap | 不要 | 不要 | 推奨 | 推奨 | 推奨 |
| アプリ側 `Worker` patch | 不要 | 不要 | 推奨 | N/A | N/A |
| アプリ側 `noInitialRun` + `callMain` | 不要 | 不要 | 不要 | **必須** (`source/main.cpp` 初期化用) | 不要 |

3 変種とも `script/wasm_build.js` が同じソースから一度のビルド呼び出しで
自動生成する。Makefile 側の差分は `EM_ENVIRONMENT` /
`EM_EXPORTED_RUNTIME_METHODS` / `EM_PTHREAD` の 3 変数のみ。

「推奨」「必須」のセルは `docs/wasm_eval_results.md` の進捗と連動して
更新する。

---

## 9. edge 変種 (V8 Isolate 系ランタイム向け)

Cloudflare Workers / Vercel Edge Functions / Deno Deploy 等、
`Worker` コンストラクタと `SharedArrayBuffer` が使えない V8 Isolate
型ランタイム向けの single-thread ビルド。対応ターゲット:

- Cloudflare Workers / Cloudflare Pages Functions / Durable Objects
- Vercel Edge Functions / Next.js Edge Runtime
- Deno Deploy

### 9.1 制約

| 項目 | 挙動 |
|---|---|
| `Threads` | 1 固定。`setoption name Threads value N` は実質無視 (`source/thread.{cpp,h}` の single-thread 経路で `search()` を呼び出し元スレッドで同期実行) |
| `USI_Ponder` / `Stochastic_Ponder` | 動かない (探索と別スレッドで USI ループを回す手段が無い) |
| 探索中の `stop` / `setoption` / `position` | 送れない。`go` は呼び出しスレッドをブロックする同期呼び出し |
| `go btime/wtime/byoyomi` | `MinimumThinkingTime` / `NetworkDelay` に削られるので非推奨 |
| `go movetime N` | 推奨。ほぼ N ms で打ち切る |
| `MultiPV` / `SkillLevel` / `DepthLimit` / `NodesLimit` | 動く |
| `USI_Hash` / `EvalHash` | 動くが並列 init が無いので `isready` 応答が他変種比でやや遅め |

### 9.2 パッケージ別対応状況

edge variant は全 `pkgobj` (`halfkp` / `k-p` / `yaneuraou-mate` /
`tanuki-mate` 等) でビルド自体は通るが、実用的に edge ランタイムに
乗せられるかは wasm サイズと runtime の動作が揃っている必要がある。
2026-04-14 時点の確認結果:

| パッケージ | ビルド | smoke | wasm サイズ (br 圧縮) | edge 実用性 |
|---|---|---|---|---|
| `k-p` (KP256 NNUE) | ✅ | ✅ `go movetime 500` | 1.41 MiB (479 KiB) | **Edge 実用可** |
| `halfkp` (Suisho5+YaneuraOu NNUE) | ✅ | ✅ `go movetime 500` | **61 MiB** (25 MiB) | Edge の bundle size 上限 (Paid 10 MiB) を大きく超える。**ブラウザ直接配信なら実用可**、Workers / Edge Functions では載らない |
| `yaneuraou-mate` (df-pn mate solver) | ✅ | ⚠️ load + handshake までは通るが **`go mate` で Aborted()** | 564 KiB (113 KiB) | 現状 edge 非対応。 `df-pn` ソルバーが固有の pthread 依存を持っている疑い。web/node variant なら動く。対応は未実施 (調査保留) |
| `tanuki-mate` (mate solver) | ✅ | ✅ `go mate 2000` → `checkmate nomate` | 518 KiB (110 KiB) | **Edge 実用可**。詰将棋用途はこちらを使う |

**運用上の指針**:

- Edge で「通常思考」が欲しい → `k-p`
- Edge で「詰将棋探索」が欲しい → `tanuki-mate`
- Edge で Suisho5+YaneuraOu の強い NNUE が欲しい → **edge 不可**。
  `web` / `node` variant を直接ブラウザ or Node サーバーで使う
- `yaneuraou-mate` を edge で使いたい → 現状サポート外、将来対応は未定

### 9.3 ラッパーテンプレート

フレームワーク非依存の最小ラッパーを
`templates/edge/yaneuraou-edge.ts` に用意してある。コピペで自分の
プロジェクトに取り込む前提で、`@types/emscripten` にも依存しない。

```ts
import {
  createYaneuraOuEdge,
  type YaneuraOuFactory,
  type EvalRequest,
  type EvalResult,
} from "./yaneuraou-edge";

const engine = await createYaneuraOuEdge({
  factory,       // yaneuraou.<pkg>.js から import した default export
  wasmBinary,    // yaneuraou.<pkg>.wasm の中身 (ArrayBuffer|Uint8Array)
  usiHash: 16,   // MB (省略時 16)
  hash: 16,      // MB (省略時 usiHash と同値)
});
```

- `createYaneuraOuEdge()` の中で `usi` → `setoption Threads/Hash` →
  `isready` まで済ませる。以降は `eval()` / `evalBatch()` を繰り返し
  呼ぶだけ。
- 連続呼び出しは内部で自動直列化される。呼び出し側でロックを取る必要は無い。
- `dispose()` で `Module.terminate()`。呼び忘れても致命的ではない。

### 9.4 `eval()` と `evalBatch()` の使い分け

| | `eval(req)` | `evalBatch(reqs)` |
|---|---|---|
| 用途 | 独立した 1 局面の評価 | 連続した局面列の評価 (棋譜解析) |
| TT 状態 | 毎回 `usinewgame` でクリア | Batch 最初の 1 回だけ `usinewgame`。以降は前の局面の TT を次の局面で再利用 |
| option | 1 局面ごとに設定 | 配列先頭の要素から取り、Batch 内は固定 |
| 探索効率 | 局面ごと独立 | 同じ思考時間でも hash hit 率が上がり、実効探索深さが伸びる |
| 何件まとめていいか | — | 1 Batch = 1 USI セッション。Workers の CPU time 30 s 上限内で、`byoyomi=500 ms` なら 50〜55 局面が上限 |

```ts
// 単発 (独立した 1 局面)
const result: EvalResult = await engine.eval({
  sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
  byoyomi: 500,
  skillLevel: 20,
});

// 連続 (棋譜解析)
const results: EvalResult[] = await engine.evalBatch(
  sfens.map((sfen) => ({ sfen, byoyomi: 500 })),
);
```

棋譜解析で option を要素ごとに変えたい場合は Batch を分割する
(option 変更は実質 TT flush で Batch の旨味が消えるため、Batch 内
option 固定は意図的な制約)。

### 9.5 option の扱い (Isolate-level vs Request-level)

同じ Isolate に複数ユーザーのリクエストが届く前提なので、option を
2 層に分けている:

| option | 層 | 適用タイミング |
|---|---|---|
| `Threads` / `USI_Hash` / `Hash` | **Isolate-level** | `createYaneuraOuEdge()` で 1 度だけ。置換表の reallocate コストが高いので使い回す |
| `MultiPV` / `SkillLevel` / `DepthLimit` / `NodesLimit` | **Request-level** | `eval()` / `evalBatch()` 呼び出しのたびに毎回 `setoption` で上書き |

Request-level option は **省略時も USI 既定値で毎回上書き** されるので、
前の呼び出しで立てた値を後続リクエストが引きずらない。これは
「user A が `eval({ skillLevel: 5 })` → user B が skillLevel 指定なしで
`eval()`」というパターンで user B が user A の設定で思考される事故を
防ぐため。

### 9.6 組み込み例 (Cloudflare Workers)

wrangler で wasm と JS を bundle し、module scope にエンジンを 1 つ
持っておく構成:

```ts
// src/index.ts
import YaneuraOu_K_P from "./yaneuraou.k-p.js";
import wasmBinary from "./yaneuraou.k-p.wasm";
import {
  createYaneuraOuEdge,
  type YaneuraOuFactory,
} from "./yaneuraou-edge";

// Isolate 寿命中は同じエンジンを使い回す (cold start 時のみ初期化コスト)
const enginePromise = createYaneuraOuEdge({
  factory: YaneuraOu_K_P as unknown as YaneuraOuFactory,
  wasmBinary,
});

export default {
  async fetch(req: Request): Promise<Response> {
    const { sfen, byoyomi = 500 } = await req.json<{
      sfen: string;
      byoyomi?: number;
    }>();
    const engine = await enginePromise;
    const result = await engine.eval({ sfen, byoyomi });
    return Response.json(result);
  },
};
```

`wrangler.toml` 側で `.wasm` をバイナリとして import できるように:

```toml
[[rules]]
type = "Data"
globs = ["**/*.wasm"]
fallthrough = true
```

Vercel Edge Functions / Deno Deploy も同パターン。`yaneuraou.k-p.js`
を ES module として import し、`.wasm` を ArrayBuffer として渡すだけ。

### 9.7 実測パフォーマンス (k-p, em++ 5.0.5, aarch64)

`templates/edge/yaneuraou-edge.ts` + `go movetime 500`、中盤局面 1 手、
Node (bun):

| phase | 時間 |
|---|---|
| ① `factory(...)` ロード (WASM compile + runtime init) | ~90 ms |
| ② `usi` → `usiok` + 初回 `setoption` → `isready` → `readyok` | ~30 ms |
| ③ `go movetime 500` → `bestmove` | ~505 ms |
| **cold 合計** | **~620 ms** |
| **warm 合計** (Isolate 再利用、① ② が省略される) | **~510 ms** |

**wasm サイズ**: `yaneuraou.k-p.wasm` 約 1.41 MiB / Brotli 圧縮後
479 KiB。Workers の bundle size 上限 (Paid 10 MiB / Free 3 MiB) に対し余裕。

**CPU time**: Cloudflare Workers Paid プランの 30 s 上限に対して
1 リクエスト ~500 ms で ~2% 消費。Free プランの 10 ms CPU は当然無理
(思考だけで 500 ms 消費する)。
