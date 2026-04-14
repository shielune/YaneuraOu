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

### 9.1 対象ランタイム

V8 Isolate を JS 実行モデルとするエッジランタイムでは、`Worker`
コンストラクタと `SharedArrayBuffer` が使えないため、pthread 付きの
web/node variant はそのままでは動作しない。edge 変種はこれらを
外した single-thread ビルド。代表的なターゲット:

- **Cloudflare Workers** / **Cloudflare Pages Functions** / **Durable Objects**
- **Vercel Edge Functions** / **Next.js Edge Runtime**
- **Deno Deploy**

### 9.2 制約

| 項目 | 挙動 |
|---|---|
| `Threads` | 1 固定。`setoption name Threads value N` は実質無視 (`source/thread.{cpp,h}` が single-thread パスで search() を呼び出し元スレッドで同期実行) |
| `USI_Ponder` / `Stochastic_Ponder` | 機能しない。探索と別スレッドで USI ループを回す手段が無い |
| 探索中の `stop` / `setoption` / 次の `position` | 送れない。`go` は呼び出しスレッドをブロックする同期呼び出しとして動く |
| `go btime/wtime/byoyomi` | `MinimumThinkingTime` / `NetworkDelay` に削られるので非推奨 |
| `go movetime N` | 推奨。ほぼ N ms で打ち切る |
| `MultiPV` / `SkillLevel` / 探索系 option 全般 | 動く |
| `USI_Hash` / `EvalHash` | 動くが、並列 init が無いので `isready` 応答が変種比で遅め |

### 9.3 最小ラッパー: `templates/edge/yaneuraou-edge.ts`

フレームワーク非依存の最小ラッパーをリポジトリに用意してある。
**コピペで自分のプロジェクトに取り込んで使う前提** で、`@types/emscripten`
にも依存しない形にしてある。

提供している API:

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

const result: EvalResult = await engine.eval({
  sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
  byoyomi: 500,     // ms。内部では `go movetime` として発行
  skillLevel: 20,   // 0–20, 省略可
});
// result.bestmove      : "6i7h"
// result.ponder        : "8c8d" | null
// result.score         : { kind: "cp" | "mate", value: number, bound? }
// result.depth/nodes/pv/...
```

- `createYaneuraOuEdge()` 内で `usi` → `setoption Threads/Hash` → `isready`
  までを済ませ、以降は `eval()` の繰り返しで使い回せるようにしてある。
- **連続 `eval()` 呼び出しは内部で自動直列化される**ので、呼び出し側で
  ロックを取る必要は無い。
- `dispose()` で `Module.terminate()` を呼ぶ。Isolate が捨てられる前に
  呼ぶと綺麗だが、呼び忘れても実害は無い。

#### option の分類 (Isolate-level vs Request-level)

エッジ環境では **1 つの Isolate に複数ユーザーのリクエストが届く**
前提になるので、option を 2 層に分けて管理している:

| option | 層 | 振る舞い |
|---|---|---|
| `Threads` / `USI_Hash` / `Hash` | **Isolate-level** | `createYaneuraOuEdge()` で 1 度だけ設定。置換表の reallocate コストが高いので使い回す |
| `MultiPV` / `SkillLevel` / `DepthLimit` / `NodesLimit` | **Request-level** | `eval()` 呼び出しのたびに毎回 `setoption` で上書き。指定省略時は USI 既定値 (`MultiPV=1` / `SkillLevel=20` / `DepthLimit=0` / `NodesLimit=0`) で上書きする |

Request-level option は **省略された場合も既定値で上書き** されるので、
前の `eval()` で設定された値が後続リクエストに漏れない。これは
「user A が SkillLevel=5 で `eval()` → user B が SkillLevel を指定せずに
`eval()`」というパターンで、user B が user A の設定で思考されてしまう
事故を防ぐため。

### 9.4 Cloudflare Workers への組み込み例

wrangler で wasm と JS を bundle して、module scope にエンジンを
1 個持っておく構成:

```ts
// src/index.ts
import YaneuraOu_K_P from "./yaneuraou.k-p.js";
import wasmBinary from "./yaneuraou.k-p.wasm"; // wrangler.toml で data 扱い
import {
  createYaneuraOuEdge,
  type YaneuraOuFactory,
} from "./yaneuraou-edge";

// Isolate 寿命中は同じエンジンを使い回す。cold start 時のみ初期化コストを払う。
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

`wrangler.toml` 側で `.wasm` をバイナリとして import できるよう
`rules` を設定する (Workers の場合):

```toml
[[rules]]
type = "Data"
globs = ["**/*.wasm"]
fallthrough = true
```

Vercel Edge Functions / Deno Deploy も同様のパターンで動く。
`yaneuraou.k-p.js` を ES module として import し、`.wasm` を
ArrayBuffer として渡すだけで差し替えられる。

### 9.5 実測パフォーマンス (k-p, em++ 5.0.5, aarch64)

`templates/edge/yaneuraou-edge.ts` + `go movetime 500` で中盤局面
1 手、Node (bun) で実行:

| phase | 時間 |
|---|---|
| ① `factory(...)` ロード (WASM compile + runtime init) | ~90 ms |
| ② `usi` → `usiok` + 初回 `setoption` → `isready` → `readyok` | ~30 ms |
| ③ `go movetime 500` → `bestmove` | ~505 ms |
| **cold 合計 (1 リクエスト目)** | **~620 ms** |
| **warm 合計 (Isolate 再利用)** | **~510 ms** (①② が省略される) |

Cloudflare Workers Paid プランの **CPU time 30 s 上限** に対して ~2%
の消費で収まる。Free プランの 10 ms CPU は当然無理 (思考だけで
500 ms 消費する)。

wasm サイズ: `yaneuraou.k-p.wasm` が約 **1.41 MiB**、Brotli 圧縮後で約
**479 KiB**。Workers の bundle size 上限(Paid 10 MiB / Free 3 MiB)に対して
十分な余裕がある。
