# yaneuraou-wasm-mate-pthread — internal spec for LLM consumers

This document describes the internal contract between the Wasm binary, the
TypeScript loader, and the consumer code. Sibling spec:
`yaneuraou-wasm-mate-cfworkers/SPEC.md` (single-thread variant).

## Wasm binary

- Source: `source/engine/yaneuraou-mate-engine/yaneuraou-mate-search.cpp`
- Build edition: `YANEURAOU_EDITION=YANEURAOU_MATE_ENGINE`
- Linker flags (em++):
  - `EM_EXPORT_NAME=YaneuraOu_MATE`
  - `EM_ENVIRONMENT=web,worker`
  - `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall','HEAPU8']`
  - `EM_PTHREAD=1`
  - `PTHREAD_POOL_SIZE=32` (hard-coded in `source/Makefile`; the pool is
    pre-spawned even though DfPn only uses one worker — over-provisioning
    is intentional so `Threads` USI option can grow without a relink)
  - `EM_INITIAL_MEMORY_SIZE=134217728` (128 MB)
  - `EM_MAXIMUM_MEMORY_SIZE=1073741824` (1 GB)
  - `EM_STACK_SIZE=2097152` (2 MB)
  - `EM_CLOSURE=0`
- Emscripten: 5.0.5 (docker image `emscripten/emsdk:5.0.5`)

## Multi-thread variant

`source/engine/yaneuraou-mate-engine/yaneuraou-mate-search.cpp` defines
`MainThread::search()` twice — once guarded by
`#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)` (the
single-thread variant) and once for normal pthread builds. This package uses
the latter.

The pthread variant launches a `std::thread` running
`solver.mate_dfpn(rootPos, NodesLimit)` and polls for completion every 100 ms,
emitting periodic `info ... pv ...` lines. Termination is by:

- proof completion (`mate` / `nomate`),
- `Search::Limits.mate` (the `<ms>` from `go mate <ms>`) — produces
  `checkmate timeout`,
- `NodesLimit` exhaustion or OOM — produces `checkmate none`.

## Cross-origin isolation

The WASM module spawns Web Workers from a Blob URL backed by `yaneuraou.js`.
Browsers gate `SharedArrayBuffer` (and therefore Emscripten pthreads) behind
the cross-origin isolation flag, so the embedding page must be served with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

If your bundler does not preserve `import.meta.url` for the engine `.js`,
pass `mainScriptUrlOrBlob` to `createMateEngine` so the worker can find the
script.

## USI surface

Identical to `yaneuraou-wasm-mate-cfworkers/SPEC.md` except:

- `setoption name Threads value <N>` is honored. The DfPn solver itself is
  not parallelised, so values > 1 do not speed up search. Default 1.
- `go mate <ms>` time limit is honored; `checkmate timeout` is possible.

## TypeScript loader (`src/index.ts`)

`createMateEngine(opts)` adds a runtime check for `SharedArrayBuffer` and
forwards `opts.mainScriptUrlOrBlob` to the Emscripten factory if provided.
Otherwise the surface is identical to the cfworkers loader.

## Memory expectations

- Engine boot: 128 MB heap (the WASM `INITIAL_MEMORY`).
- After `isready` with `USI_Hash=1MB`: heap stays at ~140 MB.
- After `isready` with `USI_Hash=64MB`: heap grows to ~205 MB.
- `EM_MAXIMUM_MEMORY_SIZE` caps growth at 1 GB; `SHARED_MEMORY` (Emscripten's
  SharedArrayBuffer-backed heap) imposes a hard ceiling per tab.

## Performance expectations

DfPn is not parallelised; performance per tsume problem is comparable to the
single-thread cfworkers build, with the extra cost of pthread context switches.
The win of this build is that the JS main thread stays responsive (DfPn runs
in a worker), which lets the host page show progress UI and honor
`go mate <ms>` deadlines.
