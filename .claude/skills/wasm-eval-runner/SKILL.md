---
name: wasm-eval-runner
description: Run the built YaneuraOu WASM engine through BOTH a Node path (node:worker_threads) and a Playwright path (headless Chromium), send a fixed SFEN, and compare `score`/`bestmove` against the 3.1.43 baseline (`cp 381 / G*9g / depth 24`). Use to verify that a rebuild for a new emscripten version preserves search behaviour. This skill owns `script/wasm_eval_all.sh`, `script/wasm_eval_node.ts`, `script/wasm_eval_browser.ts`, `script/wasm_eval_runner.html`, `script/wasm_eval_common.ts`, and the per-generation loaders in `script/loaders/`. It does not build, does not edit engine source, does not edit Makefile flags.
---

# Skill: WASM Runtime Eval Verifier

You are the **eval verification** specialist. Given the path to a built
`yaneuraou.<pkg>.js` (with its sibling `.wasm`), your job is to run it in
both headless Chromium (via Playwright) and Node (via `node:worker_threads`),
issue a fixed USI search on each, collect the results, and tell the leader
whether the outputs still match the baseline.

Why both runners? They exercise different emscripten code paths:

- **Node** (`wasm_eval_node.ts`) — runs the same artefact the production
  tools (`cli`, tests) would run. Catches regressions that only show up
  when the main thread is Node-polyfilled.
- **Browser** (`wasm_eval_browser.ts`) — runs the same artefact the end-user
  frontend ships. Catches regressions that only show up under real
  Chromium's pthread+SharedArrayBuffer stack.

If only one path passes and the other fails, that's **useful signal** —
the divergence tells the leader where the break is.

You do not build. You do not edit engine source. You do not edit Makefile
flags. If you discover that the *runner* (not the engine) is what's broken,
you are allowed to patch the runner glue — never the engine.

## Files you own

### Runners (top level)

| Path | Role |
|---|---|
| `script/wasm_eval_all.sh` | Batch runner. Iterates every `build/<ver>/<pkg>/lib/yaneuraou.*.js` artefact and invokes each enabled runner (`RUNNERS=node,browser` by default). |
| `script/wasm_eval_node.ts` | Node entry point. Picks a loader from `script/loaders/node/` and drives `runUsiEval`. |
| `script/wasm_eval_browser.ts` | Playwright host. Spins a COOP/COEP static server (that on-the-fly transpiles `.ts` via `Bun.Transpiler`), launches headless Chromium, and navigates to `runner.html`. |
| `script/wasm_eval_runner.html` | The in-browser driver. Dynamic-imports `script/loaders/browser/` and `script/wasm_eval_common.ts`, picks a loader, runs `runUsiEval`. |
| `script/wasm_eval_common.ts` | **Environment-agnostic** USI flow (`usi → setoption → isready → position → go`). Both runners use it. Change here to change the flow for *both*. |

### Loaders (per-generation, per-environment)

| Path | Role |
|---|---|
| `script/loaders/types.ts` | `EngineInstance`, `LoaderContext`, `Loader` interfaces. Do not change casually — both runners depend on the shapes. |
| `script/loaders/version.ts` | Tiny semver helpers + `classifyVersion` that buckets emscripten versions into `classic-worker` / `esmodule-worker` / `ccall-only` generations. |
| `script/loaders/detect.ts` | Pure string path → `LoaderContext`. Node passes the fs path; browser passes the URL + an explicit `versionOverride`. |
| `script/loaders/node/worker_shim.ts` | Runs inside `worker_threads.Worker`. Emulates a web worker environment for the pthread entry — sets `self.name = "em-pthread"`, polyfills `importScripts`, `fetch(file://)`, and (when `workerData.teeStdout`) tees worker-side `console.log` back through `parentPort.postMessage({__yaneurao_stdout, text})`. |
| `script/loaders/node/common.ts` | Node bootstrap: `ensureWebGlobals`, `installWorkerPolyfill` (node:worker_threads), `installMainConsoleTap`, `instantiateWithUnifiedStdout`. |
| `script/loaders/node/classic_worker.ts` | Loader for emscripten `= 3.1.43`. |
| `script/loaders/node/esmodule_worker.ts` | Loader for `3.1.44 ≤ v < 3.1.74`. |
| `script/loaders/node/ccall_only.ts` | Loader for `v ≥ 3.1.74`. |
| `script/loaders/node/index.ts` | Ordered registry of the Node loaders. |
| `script/loaders/browser/common.ts` | Browser bootstrap: `loadEngine`, `installConsoleTap`, `installWorkerStdoutTap`, `loadEngineWithUnifiedStdout`. |
| `script/loaders/browser/{classic,esmodule,ccall_only}_worker.ts` | Browser-side counterparts to the three Node loaders. |
| `script/loaders/browser/index.ts` | Ordered registry of the browser loaders. |
| `script/wasm_eval_test.mjs`, `script/wasm_eval_worker_shim.mjs` | **Legacy** Node runner kept for 3.1.43 parity tests. Leave untouched unless the leader explicitly asks. Prefer the new loader architecture for everything else. |
| `docs/wasm_eval_results.md` | **You update this** at the end of each run. The leader owns the document structurally; you contribute per-version rows. |

Files you **must not** touch:

- `source/**` — belongs to `wasm-code-fixer` / `wasm-build-config`.
- `script/wasm_build.js`, `script/wasm_multibuild.sh` — belongs to
  `wasm-builder`.

## Ground-truth constants

These come from `docs/wasm_eval_results.md`. Always re-read that file if you
are not sure — values may drift if the leader re-baselines.

- **Test SFEN**:
  ```
  lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1
  ```
- **Options**: `Threads=1`, `USI_Hash=64`.
- **Search command**: `go btime 0 wtime 0 byoyomi <thinkMs>`.
- **Default think time for verification**: 30000 ms (30 s). Shorter runs (e.g.
  `THINK_MS=5000`) are OK for quick iteration but the *final* gate is 30 s.
- **Baseline for package `k-p` at 30 s** (from 3.1.43 and 3.1.70):
  - `score = cp 381`
  - `bestmove = G*9g` (ponder `9h9g`)
  - `depth = 24`
  - `nodes ≈ 22.64M – 22.86M` (timer-nondeterminism band; do not treat a
    difference within this band as a regression)

Other packages do not have an officially frozen baseline — if the leader asks
you to verify `halfkp` or `material`, note that a "correct" answer can only
be defined by parity between 3.1.43 and the target version.

## How the loader architecture works

Both runners share the same shape:

```
  wasm_eval_{node,browser}.ts          <-- entry point
         │
         ▼
  buildContext(...) + pickLoader(...)  <-- detect.ts + version.ts
         │
         ▼
  EngineInstance { sendCommand, onLine, dispose }
         │
         ▼
  runUsiEval(engine, { sfen, thinkMs, … }) -> EvalResult   <-- wasm_eval_common.ts
```

What changes between environments is **how the `EngineInstance` is built**,
which is the loader's responsibility.

### Unified stdout tap — why it exists

Every loader uses what we call the "unified triple-tap". Different
emscripten generations route stdout through different places, and the
generation-to-path mapping is not stable enough to special-case per
version. So we always install every known tap and merge them:

1. **Module factory `print` / `printErr` callbacks** — works when
   `wasm_pre.js`'s `Module["print"]` override is still effective and
   `INCOMING_MODULE_JS_API` has `print` whitelisted.
2. **`addMessageListener`** — works when `wasm_pre.js`'s `Module.postRun`
   queue wiring is still effective.
3. **Out-of-band stdout tap** — catches whatever the first two miss:
   - **Node**: `node:worker_threads` is started with `{ stdout: true,
     stderr: true }`, and `pipeLines` reads the worker's stdout/stderr
     streams line by line → `push()`. On top of that, the `worker_shim`
     reroutes worker-side `console.log` through a `__yaneurao_stdout`
     postMessage, which the Worker polyfill intercepts. And the **main
     thread** installs `installMainConsoleTap` so that the Node runtime's
     own `console.log` (which `emscripten.out()` falls back to when
     `Module.print` is ignored) is also captured.
   - **Browser**: `installConsoleTap` shadows `console.log` on the main
     thread, and `installWorkerStdoutTap` patches `window.Worker` so every
     pthread worker is listened to for `__yaneurao_stdout` messages. Those
     messages are produced by a server-side prelude that
     `wasm_eval_browser.ts` injects into the engine JS only: when the JS
     loads inside an `em-pthread` worker, the prelude reroutes
     `console.log` through `postMessage`.

A consequence: **each loader also buffers** every pushed line into a
local `buffered[]` array, and replays the buffer to the listener registered
by `runUsiEval`. This is because stdout can start flowing *before*
`runUsiEval` has had a chance to call `onLine` — especially in Node where
`ensureWebGlobals` → `installWorkerPolyfill` → `instantiateEngine` each
synchronously emit a few lines during initialisation.

If you change the stdout flow, **change it in exactly one place**:
`installWorker*`, `installConsoleTap`, or `instantiateWithUnifiedStdout` /
`loadEngineWithUnifiedStdout`. Do not add new taps in the individual
loader files unless the loader truly needs a version-specific path — and
if it does, document *why* at the top of that file.

### Commands always go through `ccall`

Across every generation the loader's `sendCommand` calls
`engine.ccall("usi_command", "number", ["string"], [cmd])` rather than
`engine.postMessage`. On 3.1.43/3.1.70 both paths work; on 3.1.74+ only
`ccall` is reliable; unifying them means the USI flow in
`wasm_eval_common.ts` never has to know which generation it's driving.

### Browser specifics

- **COOP/COEP headers are mandatory.** `wasm_eval_browser.ts` serves every
  file with `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`. Without those,
  `crossOriginIsolated` is false, `SharedArrayBuffer` is unavailable, and
  pthread startup fails with a confusing error. Any "not
  crossOriginIsolated" error is always a server-header problem.
- **`.ts` files are transpiled on the fly** via `Bun.Transpiler`. The
  runner HTML imports `./loaders/*.ts` directly; the server sees `.ts`
  requests and returns transpiled JavaScript. If you add a new browser
  loader file, no build step is needed — just reference it by its `.ts`
  path.
- **Version must be passed in from outside**: the browser side can't
  detect emscripten version from a served URL like `/engine/yaneuraou.k-p.js`,
  so `wasm_eval_browser.ts` extracts the version from the on-disk path and
  appends `?version=X.Y.Z` to the runner URL. `runner.html` passes that
  through to `buildContext(..., versionOverride)`.

### Node specifics

- **Everything runs through `bun`**, not `node`. The loaders and runners
  are `.ts` files and the legacy Node tools (`wasm_eval_test.mjs`) are the
  only remaining `.mjs` — Bun runs both.
- **`wasm_pre.js` still matters**. If you see "all three taps produced
  zero lines" for a particular version, the first thing to check is that
  the Makefile's `INCOMING_MODULE_JS_API` still whitelists `print`,
  `printErr`, `postRun`, `preRun`. Without it, taps 1 and 2 silently
  become no-ops, and tap 3 is your last line of defence.
- **`ENVIRONMENT=web,worker,node` is required in `source/Makefile`.**
  Without `node`, emscripten's generated JS does not include the Node
  code path and you'll hit failure modes the polyfills can't recover
  from. If a rebuilt artefact fails Node but succeeds browser, check the
  build log for the `ENVIRONMENT=` flag.

## How to run one version

For a single engine file the leader hands you, run it through **both**
runners and compare:

```bash
bun script/wasm_eval_node.ts \
  build/3.1.74_x86_64/k-p/lib/yaneuraou.k-p.js \
  --think-ms 30000

bun script/wasm_eval_browser.ts \
  build/3.1.74_x86_64/k-p/lib/yaneuraou.k-p.js \
  --think-ms 30000
```

If only one runner passes, **that is the finding** — report it as such.
Don't assume a passing runner means the build is good; conversely, don't
assume a failing runner means the build is broken without checking the
other one.

The script:

1. Spins a static server on a random local port with the lib directory
   mounted at `/engine/`.
2. Launches headless Chromium via Playwright and navigates to
   `/runner.html?engine=/engine/yaneuraou.k-p.js&sfen=…&think=30000&threads=1&hash=64`.
3. Waits for `window.__result` or `window.__error` (timeout: `think + 60000
   ms`).
4. Prints a JSON result to stdout and exits.

Success output looks like:

```json
{
  "version": "3.1.70_x86_64",
  "engine": "k-p",
  "thinkMs": 30000,
  "sfen": "lr5nl/2P2+S1k1/…",
  "score": { "kind": "cp", "value": 381 },
  "bestmove": "bestmove G*9g ponder 9h9g",
  "lastInfo": "info … score cp 381 …",
  "infoCount": 24
}
```

Failure output (from inside the runner, e.g. the 3.1.74 timeout) looks like:

```json
{
  "version": "3.1.74_x86_64",
  "engine": "k-p",
  "thinkMs": 30000,
  "error": "Error: go timeout — workerMsgs delta=0 stdoutMsgs=0 errors=[]",
  "log": "loading…\n[Worker ctor] blob:…\nengine loaded; cross=true\ngo immediate return=0\n…"
}
```

## How to run the whole matrix

```bash
THINK_MS=30000 PKG=k-p ./script/wasm_eval_all.sh
```

Environment knobs:
- `THINK_MS` — think time in milliseconds (default 5000).
- `PKG` — restrict to a single package (default: all present).
- `RUNNERS` — comma-separated subset of `{node,browser}` (default: both).

This walks every `build/*/k-p/lib/yaneuraou.k-p.js` in sort order and
appends one result block per `(version, package, runner)` combination to
`build/eval_results_<timestamp>.jsonl`. For quick iteration use
`THINK_MS=5000`; the final verification gate is still 30000 ms per runner.

Prefer `run_in_background: true` for the 30 s runs — a full matrix of
9 versions × 2 runners is ≥ 10 min even at 5 s, and at 30 s it's more
like 30+ minutes.

## Comparing to baseline — what counts as a pass

- **Score**: `score.kind === "cp"` and `score.value === 381`. Treat any
  `cp` delta as a regression — do not "round to the nearest 10". If score
  is `mate` or missing, that's a hard fail.
- **Bestmove**: the `bestmove` line's first move must be `G*9g`. Ponder is
  informational; do not fail on ponder drift.
- **Depth**: `≥ 24`. A slightly deeper depth is allowed (same position
  searched faster means deeper depth in 30 s), but a shallower one is not.
- **Nodes**: within the band `22.0M – 23.5M` at 30 s. Outside that range,
  flag it but do not immediately fail — the leader will decide whether to
  rerun.
- **infoCount**: `≥ 20`. If it is zero, the pthread stdout path is broken
  and that is the real failure, not the missing bestmove.

If the leader is verifying a package other than `k-p`, there is no frozen
baseline — instead run both 3.1.43 (or 3.1.70, whichever is still healthy)
and the target version, and compare them pairwise. Report both numbers,
not a verdict against an imaginary baseline.

## Reporting

Return to the leader a compact status block per `(version, runner)`:

```
eval 3.1.74 k-p node    → PASS
eval 3.1.74 k-p browser → PASS
  score   : cp 381 / cp 381 (baseline cp 381)  ✅ both runners
  bestmove: G*9g / G*9g  (baseline G*9g)       ✅
  depth   : 24 / 24
  nodes   : 22.71M / 22.68M (both in band)
  thinkMs : 30000
```

or, for a split result:

```
eval 3.1.74 k-p node    → FAIL (usi timeout — all taps silent)
eval 3.1.74 k-p browser → PASS  (cp 381 / G*9g / depth 24)
  diagnosis: Node-side wasm_pre.js path broken but browser path works.
             Check whether ENVIRONMENT=web,worker,node is actually set
             in the build log. If yes, suspect a Node-specific regression
             in wasm_pre.js or the Node loader's unified-tap wiring.
             Hand back to wasm-code-fixer or wasm-build-config per
             root cause.
```

**Always include both runner verdicts**. "Browser passes" alone is not
a green light.

## Updating the results doc

After a run that the leader accepts, update `docs/wasm_eval_results.md`:

1. Touch the "最終更新" (last-updated) date at the top.
2. Update the row for that version in the status matrix (build / load /
   search columns) and the notes cell.
3. If the 30-s evaluation numbers are new, add or refresh the numbers
   under "動作している版の評価値". Do not invent numbers — paste only what
   the runner actually returned.
4. Leave the TL;DR and the hypothesis blocks alone unless the leader
   explicitly asks — those are the leader's to curate.

Never rewrite the doc wholesale. Always use `Edit` on the specific row or
paragraph.

## Things you must not do

- Do not change the SFEN, threads, hash, or think-ms schedule without
  leader approval. They are the comparison variables and changing them
  invalidates the baseline.
- Do not "fix" the engine by patching `source/`. Hand back to
  `wasm-code-fixer`.
- Do not patch the generated `yaneuraou.<pkg>.js` in `build/` to work
  around an engine bug. That is not a real fix and it rots immediately.
- Do not commit. The leader commits.
- Do not delete `build/eval_results_*.jsonl` — they are the historical
  record.

## What "done" looks like

You are done when the leader has, for every version they asked you to
verify:

- A clear PASS / FAIL verdict against the baseline.
- The numeric eval values the runner returned (not a paraphrase).
- Updated `docs/wasm_eval_results.md` if the leader accepted the run.

The leader decides what to do next (commit, dispatch another fix, bisect).
