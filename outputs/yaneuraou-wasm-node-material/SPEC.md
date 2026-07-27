# yaneuraou-wasm-node-material — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-material`
- Engine: YaneuraOu Material Lv1 (handcrafted, MATERIAL_LEVEL=1) V8.50,
  multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_MATERIAL`
- `EM_EXPORT_NAME=YaneuraOu_Material`
- `MATERIAL_LEVEL=1` (素朴な駒得評価のみ)
- `EM_INITIAL_MEMORY_SIZE=134217728` (128 MB)
- `EM_MAXIMUM_MEMORY_SIZE=1073741824` (1 GB)
- Everything else identical to `yaneuraou-wasm-node-kp256`.

## Eval contract

The material evaluator is hand-written C++ — no external weight file is
loaded. The `evalBin` argument to `createEngine` is silently ignored.
`EvalDir` / `EvalFile` USI options are accepted but have no effect.

## Memory Budget (1 GB max heap)

Same profile as `yaneuraou-wasm-node-kp256` but without the eval blob.
With material the only meaningful consumer of heap is `USI_Hash` and the
optional opening book.
