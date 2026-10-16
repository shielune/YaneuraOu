# yaneuraou-wasm-pthread-ka2048 — internal spec for LLM consumers

This document describes the internal contract between the Wasm binary, the
TypeScript loader, and the consumer code. Sibling spec:
`yaneuraou-wasm-cfworkers/SPEC.md` (single-thread variant for Cloudflare Workers).

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-pthread-ka2048`
- Engine: YaneuraOu SFNN ka2 2048x2-8-64 (つようら王11 micro), multi-threaded
- Runtime: browsers with cross-origin isolation (COOP/COEP)
- License: GPL-3.0

## Module Exports

```
"."        -> dist/index.js     (createEngine, types)
"./engine" -> dist/yaneuraou.js (Emscripten factory)
"./wasm"   -> dist/yaneuraou.wasm (WASM binary)
```

## Wasm binary

- Source: `source/` (engine: `YANEURAOU_ENGINE_SFNN_ka2_2048_8_64_progress8x`)
- NNUE architecture: `SFNN ka2 2048x2-8-64` / LayerStack 8 (つようら王11 micro 互換)
- Linker flags (em++):
  - `EM_EXPORT_NAME=YaneuraOu_Tuyoura11Micro`
  - `EM_ENVIRONMENT=web,worker`
  - `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall']`
  - `EM_PTHREAD=1`
  - `EM_PTHREAD_POOL_SIZE=8` (workflow matrix; passed to `PTHREAD_POOL_SIZE`)
  - `EM_INITIAL_MEMORY_SIZE=268435456` (256 MiB)
  - `EM_MAXIMUM_MEMORY_SIZE=4294967296` (4 GiB)
  - `EM_STACK_SIZE=16777216` (16 MiB per thread)
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

Identical to `yaneuraou-wasm-cfworkers/SPEC.md` except:

- `setoption name Threads value <N>` is honored. The loader defaults to 1.
  The workflow pre-spawns 8 pthread workers; keep `Threads` within this pool
  and the available memory. Larger pools require a different build.
- The configured linear-memory ceiling is 4 GiB, not a guarantee that a
  browser or device can allocate it. Hash size and thread histories consume
  this same memory budget.
- Eval must follow the `SFNN ka2 2048x2-8-64` layout — HalfKP 系や
  KP256 の重みは互換性なし。やねうら王 FANBOX 公開の
  `nn.bin` を `EvalDir` 配下に書き出して使う。
- **`progress.bin` も必須。** LayerStack の選択に使う進行度係数が `nn.bin`
  ではなく別ファイルにあるため、`progressBin` で渡して `EvalDir` 配下に
  置く (`LS_PROGRESS_COEFF` の既定名が `progress.bin`)。
- `FV_SCALE` はビルド既定 28 だが、つようら王11 は 24 前提。`createEngine` の `fvScale`
  (既定 24) が `setoption name FV_SCALE` を送る。
- bucket は進行度のみで 0〜7 を選ぶ (`progress8x`)。`LS_BUCKET_MODE` は無い。

## Initialization Sequence

1. `factory({ wasmBinary, mainScriptUrlOrBlob? })` — instantiate WASM module.
   Pthread workers are pre-spawned from the pool at this point.
2. `FS.writeFile("/eval/nn.bin", ...)` — write eval to MEMFS.
3. `FS.writeFile("/eval/progress.bin", ...)` — write progress coefficients.
4. `FS.writeFile("/book/user_book1.db", ...)` — write book to MEMFS (optional).
5. `postMessage("usi")` — USI handshake, wait for `usiok`.
6. `setoption name Threads value <N>`
7. `setoption name USI_Hash value <MB>`
8. `setoption name FV_SCALE value 24` (or the explicit `fvScale` override)
9. `setoption name EvalDir value /eval`
10. `setoption name BookDir value /book` (if book provided)
11. `setoption name BookFile value <name>` (if book provided)
12. `postMessage("isready")` — loads NNUE weights + progress coefficients +
    book + hash, wait for `readyok`.

## Behavioral Notes

- `eval()` sends `usinewgame` + `isready` before each call (clears TT).
- `evalBatch()` reuses TT across positions in the batch.
- Concurrent `eval()` calls are automatically serialized via promise chain.
- Multi-threaded search can be stopped mid-search via `stop` (raw USI), but
  `eval()` does not currently expose this.

## Memory Budget (4 GiB configured max linear memory)

| Component | Size |
|---|---|
| Initial linear-memory capacity | 256 MiB (not an extra component to add) |
| nn.bin | 3,864,610 bytes |
| progress.bin | 1,003,104 bytes |
| Evaluation files combined | 4,867,714 bytes |
| `USI_Hash` example | 64 MiB |
| Per-thread stack | 16 MiB × active pthreads |

MEMFS files, decoded NNUE parameters, temporary loading copies, thread-local
and shared histories, optional books, and other engine allocations consume
linear memory. JavaScript-side input buffers are additional host memory.
The actual peak and remaining headroom have not been measured; no fixed
thread/hash combination is guaranteed to fit a particular device.

## Error Handling

- Eval file missing: `exit(1)` during `isready`.
- `progress.bin` missing: `isready` fails — the bucket cannot be computed.
- Wrong eval architecture (KP256 into this engine): `exit(1)` during `isready`.
- Missing `SharedArrayBuffer`: module initialization fails; the async
  `createEngine` call rejects rather than producing a usable engine.
- Worker spawn failure (no COOP/COEP, `mainScriptUrlOrBlob` unresolved):
  initialization can fail or time out during the USI handshake.
