# yaneuraou-wasm-node-halfkp128 — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-halfkp128`
- Engine: YaneuraOu NNUE HalfKP_128x2_32_32 V9.60, multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_NNUE`
- `EM_EXPORT_NAME=YaneuraOu_HalfKP128`
- `EM_INITIAL_MEMORY_SIZE=268435456` (256 MB)
- `EM_MAXIMUM_MEMORY_SIZE=2147483648` (2 GB)
- Everything else identical to `yaneuraou-wasm-node-kp256` (emscripten 3.1.43,
  EM_ENVIRONMENT=node, EM_PTHREAD=1, callMain export, PTHREAD_POOL_SIZE=32).

## Memory Budget (2 GB max heap)

| Component | Size |
|---|---|
| WASM initial heap | 256 MB |
| nn.bin (HalfKP eval) | ~31 MB |
| 100T-shock book | ~5 MB |
| 700T-shock book | ~32 MB |
| `USI_Hash` 128 MB | 128 MB |
| Engine internal | ~5 MB |
| Per-thread stack (2 MB × N) | 2 N MB |
| **Headroom @ 8 threads, 128 MB hash** | ~1.6 GB |

The HalfKP eval blob does not fit in the cfworkers heap (128 MB) which is
why no cfworkers variant exists; here it fits with plenty of room for
hash + book.
