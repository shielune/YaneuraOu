# yaneuraou-wasm-node-mobility — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-mobility`
- Engine: YaneuraOu Mobility/KIKI (164-dim learned linear) V8.50,
  multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_KIKI`
- `EM_EXPORT_NAME=YaneuraOu_Mobility`
- `EM_INITIAL_MEMORY_SIZE=134217728` (128 MB)
- `EM_MAXIMUM_MEMORY_SIZE=1073741824` (1 GB)
- Everything else identical to `yaneuraou-wasm-node-kp256`.

The weights live in `source/eval/kiki/mobility_weights_embedded.cpp`,
which the KIKI edition's source list compiles in unconditionally — so
the eval is part of the Wasm binary, no MEMFS plumbing involved.

## Eval contract

External `nn.bin` is not supported. `evalBin` argument to `createEngine`
is silently dropped. `EvalDir` / `EvalFile` USI options are accepted but
have no effect.

## Memory Budget (1 GB max heap)

Same profile as `yaneuraou-wasm-node-material` — no external eval blob,
heap is mostly `USI_Hash` + optional book + per-thread stacks.
