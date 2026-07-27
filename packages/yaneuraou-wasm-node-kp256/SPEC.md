# yaneuraou-wasm-node-kp256 — internal spec for LLM consumers

This document describes the internal contract between the Wasm binary, the
TypeScript loader, and Node consumers. Sibling specs:
`yaneuraou-wasm-pthread-kp256/SPEC.md` (browser pthread variant) and
`yaneuraou-wasm-kp256-cfworkers/SPEC.md` (single-thread V8 Isolate variant).

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-kp256`
- Engine: YaneuraOu NNUE KP256 V8.50, multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Module Exports

```
"."        -> dist/index.js     (createEngine, types)
"./engine" -> dist/yaneuraou.js (Emscripten factory, ESM)
"./wasm"   -> dist/yaneuraou.wasm (WASM binary)
```

`dist/yaneuraou.worker.js` is also shipped (required at runtime by the
3.1.43 classic-worker pthread runtime) but is not subpath-exported because
emscripten loads it implicitly via `locateFile`, never via a user import.

## Wasm binary

- Source: `source/` (engine: `YANEURAOU_ENGINE_NNUE_KP256`)
- Linker flags (em++):
  - `EM_EXPORT_NAME=YaneuraOu_K_P`
  - `EM_ENVIRONMENT=node`
  - `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall','callMain']`
  - `EM_PTHREAD=1`
  - `PTHREAD_POOL_SIZE=32` (hard-coded in `source/Makefile`)
  - `EM_INITIAL_MEMORY_SIZE=134217728` (128 MB)
  - `EM_MAXIMUM_MEMORY_SIZE=1073741824` (1 GB)
  - `EM_STACK_SIZE=2097152` (2 MB)
  - `EM_CLOSURE=0`
- Emscripten: **3.1.43** (docker image `emscripten/emsdk:3.1.43`)
  - Confirmed Node-compatible toolchain. 3.1.44–3.1.73 stall on ESM
    worker bootstrap, 3.1.74+ stall on `INCOMING_MODULE_JS_API`. Both
    unresolved as of 2026-04-14; see `docs/wasm_client_usage.md`.

## Why three runtime-method exports

`EXPORTED_RUNTIME_METHODS` includes `callMain` (which the browser pthread
variant does NOT) because the Node loader passes `noInitialRun: true` and
triggers `main()` explicitly. Letting main() run automatically deadlocks
the subsequent `ccall("usi_command", ...)` path under Node.

If a future rebuild drops `callMain` from the export list, `createEngine`
throws synchronously rather than silently skipping engine Options / NNUE
initialization.

## Initialization Sequence

1. `factory({ wasmBinary, noInitialRun: true, print, printErr })` —
   instantiate WASM. Pthread workers are pre-spawned from the pool.
2. `engine.addMessageListener(line => buffer.push(line))` — tee
   `addMessageListener` output too. `print`/`printErr` and `addMessageListener`
   are not guaranteed to point at the same channel across emscripten
   generations, so we subscribe to both and merge.
3. `engine.callMain([])` — runs YaneuraOu Options/NNUE/Threads init,
   exits via `throw new ExitStatus` (caught, swallowed).
4. `FS.writeFile("/eval/nn.bin", evalBin)` — write eval to MEMFS.
5. `FS.writeFile("/book/user_book1.db", bookDb)` — write book (optional).
6. `postMessage("usi")` — USI handshake, wait for `usiok`.
7. `setoption name Threads value <N>`
8. `setoption name USI_Hash value <MB>`
9. `setoption name EvalDir value /eval`
10. `setoption name BookDir value /book` (if book provided)
11. `setoption name BookFile value <name>` (if book provided)
12. `postMessage("isready")` — loads NNUE weights + book + hash, wait for
    `readyok`.

## Behavioral Notes

- `eval()` sends `usinewgame` + `isready` before each call (clears TT).
- `evalBatch()` reuses TT across positions in the batch.
- Concurrent `eval()` calls are automatically serialized via promise chain.
- Multi-threaded search can be stopped mid-search via `stop` (raw USI), but
  `eval()` does not currently expose this.

## Memory Budget (1 GB max heap)

| Component | Size |
|---|---|
| WASM initial heap | 128 MB |
| nn.bin (KP256 eval) | ~1 MB |
| 100T-shock book | ~5 MB |
| 700T-shock book | ~32 MB |
| `USI_Hash` 64 MB | 64 MB |
| Engine internal | ~5 MB |
| Per-thread stack (2 MB × N) | 2 N MB |
| **Headroom @ 8 threads, 64 MB hash** | ~750 MB |

Hard limits:
- Eval: KP256 (~1 MB) only.
- 700T-shock book + 256 MB hash + 16 threads stays under 1 GB.

## Error Handling

- Eval file missing: `exit(1)` during `isready`.
- Wrong eval architecture (HalfKP into KP256 engine): `exit(1)` during `isready`.
- `callMain` missing from `EXPORTED_RUNTIME_METHODS`: `createEngine`
  throws synchronously before any postMessage.
- Worker spawn failure (`.worker.js` not located next to engine `.js`):
  emscripten prints a `locateFile` warning to stderr; `createEngine`
  hangs on `usi -> usiok timeout`. Fix: install the package from npm
  rather than copying only `dist/yaneuraou.js` / `dist/yaneuraou.wasm`.
