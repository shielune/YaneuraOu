# yaneuraou-wasm-node-mate — internal spec for LLM consumers

Sibling specs: `yaneuraou-wasm-node-kp256/SPEC.md` (NNUE search variant
with full discussion of the loader contract — read that for the shared
init sequence and behavioral notes), `yaneuraou-wasm-mate-pthread/SPEC.md`
(browser pthread variant), `yaneuraou-wasm-mate-cfworkers/SPEC.md`
(single-thread V8 Isolate variant).

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-mate`
- Engine: YaneuraOu Mate (DfPn) V8.50, multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Module Exports

```
"."        -> dist/index.js     (createEngine, types)
"./engine" -> dist/yaneuraou.js (Emscripten factory, ESM)
"./wasm"   -> dist/yaneuraou.wasm (WASM binary)
```

## Wasm binary

- Source: `source/` (engine: `YANEURAOU_MATE_ENGINE`)
- Linker flags differ from `yaneuraou-wasm-node-kp256` only in:
  - `YANEURAOU_EDITION=YANEURAOU_MATE_ENGINE`
  - `EM_EXPORT_NAME=YaneuraOu_MATE`
- Everything else identical (emscripten 3.1.43, EM_ENVIRONMENT=node,
  EM_PTHREAD=1, `callMain` in EXPORTED_RUNTIME_METHODS, 128 MB / 1 GB,
  PTHREAD_POOL_SIZE=32).

## Mate-specific contract

- `EvalDir` / `BookDir` USI options are accepted but have no effect.
  DfPn ignores eval.
- `evalBin` argument to `createEngine` is silently dropped.
- `USI_Hash` controls the DfPn proof/disproof number table size. Larger
  = deeper. 256 MB recommended for typical hand-tsume problems; the 1 GB
  ceiling can hold up to ~512 MB hash on Node.
- The `score` returned for a forced mate is `{ kind: "mate", value: N }`
  where `|N|` is the mate distance (positive = side-to-move mates).

## Memory Budget (1 GB max heap)

| Component | Size |
|---|---|
| WASM initial heap | 128 MB |
| `USI_Hash` (DfPn table) | 256-512 MB recommended |
| Engine internal | ~5 MB |
| Per-thread stack (2 MB × N) | 2 N MB |
| **Headroom @ 8 threads, 512 MB hash** | ~480 MB |
