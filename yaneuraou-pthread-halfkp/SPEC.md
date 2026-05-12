# yaneuraou-pthread-halfkp — internal spec for LLM consumers

This document describes the internal contract between the Wasm binary, the
TypeScript loader, and the consumer code. Sibling spec:
`yaneuraou-cfworkers/SPEC.md` (single-thread variant for Cloudflare Workers).

## Package Identity

- npm scope: `@ultemica/yaneuraou-pthread-halfkp`
- Engine: YaneuraOu NNUE HalfKP256 V8.50, multi-threaded
- Runtime: browsers with cross-origin isolation (COOP/COEP)
- License: GPL-3.0

## Module Exports

```
"."        -> dist/index.js     (createEngine, types)
"./engine" -> dist/yaneuraou.js (Emscripten factory)
"./wasm"   -> dist/yaneuraou.wasm (WASM binary)
```

## Wasm binary

- Source: `source/` (engine: `YANEURAOU_ENGINE_NNUE`)
- Linker flags (em++):
  - `EM_EXPORT_NAME=YaneuraOu_K_P`
  - `EM_ENVIRONMENT=web,worker`
  - `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall']`
  - `EM_PTHREAD=1`
  - `PTHREAD_POOL_SIZE=32` (hard-coded in `source/Makefile`)
  - `EM_INITIAL_MEMORY_SIZE=134217728` (128 MB)
  - `EM_MAXIMUM_MEMORY_SIZE=1073741824` (1 GB)
  - `EM_STACK_SIZE=2097152` (2 MB)
  - `EM_CLOSURE=0`
- Emscripten: 5.0.5 (docker image `emscripten/emsdk:5.0.5`)

## Cross-origin isolation

The WASM module spawns Web Workers from a Blob URL backed by `yaneuraou.js`.
Browsers gate `SharedArrayBuffer` (and therefore Emscripten pthreads) behind
the cross-origin isolation flag, so the embedding page must be served with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

If your bundler does not preserve `import.meta.url` for the engine `.js`,
pass `mainScriptUrlOrBlob` to `createEngine` so the workers can find the
script.

## USI surface

Identical to `yaneuraou-cfworkers/SPEC.md` except:

- `setoption name Threads value <N>` is honored. Values up to 32 are
  accepted without a relink (limited by `PTHREAD_POOL_SIZE`). Default 1.
- Memory ceiling is 1 GB (vs 128 MB on cfworkers). HalfKP eval still does
  not fit a typical HalfKP256 engine binary, but `USI_Hash` can be raised
  significantly (64–256 MB).

## Initialization Sequence

1. `factory({ wasmBinary, mainScriptUrlOrBlob? })` — instantiate WASM module.
   Pthread workers are pre-spawned from the pool at this point.
2. `FS.writeFile("/eval/nn.bin", ...)` — write eval to MEMFS.
3. `FS.writeFile("/book/user_book1.db", ...)` — write book to MEMFS (optional).
4. `postMessage("usi")` — USI handshake, wait for `usiok`.
5. `setoption name Threads value <N>`
6. `setoption name USI_Hash value <MB>`
7. `setoption name EvalDir value /eval`
8. `setoption name BookDir value /book` (if book provided)
9. `setoption name BookFile value <name>` (if book provided)
10. `postMessage("isready")` — loads NNUE weights + book + hash, wait for
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
| nn.bin (HalfKP256 eval) | ~60 MB |
| 100T-shock book | ~5 MB |
| 700T-shock book | ~32 MB |
| `USI_Hash` 64 MB | 64 MB |
| Engine internal | ~5 MB |
| Per-thread stack (2 MB × N) | 2 N MB |
| **Headroom @ 8 threads, 64 MB hash** | ~750 MB |

Hard limits:
- Eval: HalfKP256 (~60 MB) only.
- 700T-shock book + 256 MB hash + 16 threads stays under 1 GB.

## Error Handling

- Eval file missing: `exit(1)` during `isready`.
- Wrong eval architecture (KP256 into HalfKP256 engine): `exit(1)` during `isready`.
- Missing `SharedArrayBuffer`: `createEngine` throws synchronously before
  any postMessage.
- Worker spawn failure (no COOP/COEP, `mainScriptUrlOrBlob` unresolved):
  `"em-pthread" is not a function` errors in console; `createEngine` hangs
  on `usi -> usiok timeout`.
