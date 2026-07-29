# yaneuraou-wasm-node-nagisa — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-nagisa`
- Engine: YaneuraOu NNUE NAGISA V3.1 SFNN HalfKA_hm2_1024x2_15_64 (NAGISA V3.1) V8.50, multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_NNUE_HALFKP_768X2_16_64`
- `EM_EXPORT_NAME=YaneuraOu_NAGISA`
- `EM_INITIAL_MEMORY_SIZE=268435456` (256 MB)
- `EM_MAXIMUM_MEMORY_SIZE=2147483648` (2 GB)
- Everything else identical to `yaneuraou-wasm-node-kp256`.

## Eval-format compatibility

NAGISA V3.1 SFNN HalfKA_hm2_1024x2_15_64 (this), HalfKP_256x2_32_32 (suisho5), and KP256
(suishopetite) are three mutually incompatible NNUE architectures. Loading
the wrong one triggers `exit(1)` during `isready`.

## Memory Budget (2 GB max heap)

Same as `yaneuraou-wasm-node-halfkp256` — both pthread variants share the
256 MB / 2 GB profile.
