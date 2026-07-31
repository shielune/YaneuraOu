# yaneuraou-wasm-pthread-halfka1024 — internal spec for LLM consumers

This document describes the internal contract between the Wasm binary, the
TypeScript loader, and the consumer code. Sibling spec:
`yaneuraou-wasm-cfworkers/SPEC.md` (single-thread variant for Cloudflare Workers).

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-pthread-halfka1024`
- Engine: YaneuraOu SFNN HalfKA_hm2 1024x2-15-64 (NAGISA_V3) V9.60, multi-threaded
- Runtime: browsers with cross-origin isolation (COOP/COEP)
- License: GPL-3.0

## Module Exports

```
"."        -> dist/index.js     (createEngine, types)
"./engine" -> dist/yaneuraou.js (Emscripten factory)
"./wasm"   -> dist/yaneuraou.wasm (WASM binary)
```

## Wasm binary

- Source: `source/` (engine: `YANEURAOU_ENGINE_SFNN_halfkahm2_1024_15_64_progress8ek`)
- NNUE architecture: `SFNN HalfKA_hm2 1024x2-15-64` / LayerStack 9 (NAGISA_V3 互換)
- Linker flags (em++):
  - `EM_EXPORT_NAME=YaneuraOu_Nagisa`
  - `EM_ENVIRONMENT=web,worker`
  - `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall']`
  - `EM_PTHREAD=1`
  - `PTHREAD_POOL_SIZE=32` (hard-coded in `source/Makefile`)
  - `EM_INITIAL_MEMORY_SIZE=268435456` (256 MB)
  - `EM_MAXIMUM_MEMORY_SIZE=2147483648` (2 GB)
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

Identical to `yaneuraou-wasm-cfworkers/SPEC.md` except:

- `setoption name Threads value <N>` is honored. Values up to 32 are
  accepted without a relink (limited by `PTHREAD_POOL_SIZE`). Default 1.
- Memory ceiling is 2 GB (vs 128 MB on cfworkers). `USI_Hash` can be raised
  significantly (64–512 MB) along with the evaluator footprint.
- Eval must follow the `SFNN HalfKA_hm2 1024x2-15-64` layout — HalfKP 系や
  KP256 の重みは互換性なし。NAGISA_V3 Release のアーカイブに同梱された
  `nn.bin` を `EvalDir` 配下に書き出して使う。
- **`progress.bin` も必須。** LayerStack の選択に使う進行度係数が `nn.bin`
  ではなく別ファイルにあるため、`progressBin` で渡して `EvalDir` 配下に
  置く (`LS_PROGRESS_COEFF` の既定名が `progress.bin`)。
- `FV_SCALE` は SFNN 系ビルドでは既定 28 (それ以外は 16)。配布物の
  `eval_options.txt` の指定と同じなので手動設定は不要。
- `LS_BUCKET_MODE` で bucket 規則を選ぶ。既定 `progress8kpabs` は進行度のみで
  0〜7 を選び 9 個目を使わない (本家 NAGISA_V3 と同じ)。`progress8ek` は
  相入玉局面を 9 個目に割り当てる。差が出るのは相入玉局面のみ。

## Initialization Sequence

1. `factory({ wasmBinary, mainScriptUrlOrBlob? })` — instantiate WASM module.
   Pthread workers are pre-spawned from the pool at this point.
2. `FS.writeFile("/eval/nn.bin", ...)` — write eval to MEMFS.
3. `FS.writeFile("/eval/progress.bin", ...)` — write progress coefficients.
4. `FS.writeFile("/book/user_book1.db", ...)` — write book to MEMFS (optional).
5. `postMessage("usi")` — USI handshake, wait for `usiok`.
6. `setoption name Threads value <N>`
7. `setoption name USI_Hash value <MB>`
8. `setoption name EvalDir value /eval`
9. `setoption name BookDir value /book` (if book provided)
10. `setoption name BookFile value <name>` (if book provided)
11. `postMessage("isready")` — loads NNUE weights + progress coefficients +
    book + hash, wait for `readyok`.

## Behavioral Notes

- `eval()` sends `usinewgame` + `isready` before each call (clears TT).
- `evalBatch()` reuses TT across positions in the batch.
- Concurrent `eval()` calls are automatically serialized via promise chain.
- Multi-threaded search can be stopped mid-search via `stop` (raw USI), but
  `eval()` does not currently expose this.

## Memory Budget (2 GB max heap)

| Component | Size |
|---|---|
| WASM initial heap | 256 MB |
| nn.bin (SFNN HalfKA_hm2 eval) | ~75 MB |
| progress.bin | ~1 MB |
| 100T-shock book | ~5 MB |
| 700T-shock book | ~32 MB |
| `USI_Hash` 64 MB | 64 MB |
| Engine internal | ~5 MB |
| Per-thread stack (2 MB × N) | 2 N MB |
| **Headroom @ 8 threads, 64 MB hash** | ~750 MB |

Hard limits:
- Eval: SFNN HalfKA_hm2 (~75 MB) only, plus `progress.bin`.
- 700T-shock book + 256 MB hash + 16 threads stays under 1 GB.

## Error Handling

- Eval file missing: `exit(1)` during `isready`.
- `progress.bin` missing: `isready` fails — the bucket cannot be computed.
- Wrong eval architecture (KP256 into this engine): `exit(1)` during `isready`.
- Missing `SharedArrayBuffer`: `createEngine` throws synchronously before
  any postMessage.
- Worker spawn failure (no COOP/COEP, `mainScriptUrlOrBlob` unresolved):
  `"em-pthread" is not a function` errors in console; `createEngine` hangs
  on `usi -> usiok timeout`.
