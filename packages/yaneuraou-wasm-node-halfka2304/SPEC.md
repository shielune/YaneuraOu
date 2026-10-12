# yaneuraou-wasm-node-halfka2304 — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-halfka2304`
- Engine: YaneuraOu SFNN HalfKA_hm2 2304x2-15-64 (NAGISA_V4) V9.60, multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_SFNN_halfkahm2_2304_15_64_progress8ek`
- `EM_EXPORT_NAME=YaneuraOu_Nagisa4`
- `EM_INITIAL_MEMORY_SIZE=536870912` (512 MB)
- `EM_MAXIMUM_MEMORY_SIZE=4294967296` (4 GB)
- Everything else identical to `yaneuraou-wasm-node-kp256`.

## Eval-format compatibility

SFNN HalfKA_hm2 (this), HalfKP_768x2_16_64 (aoba), HalfKP_256x2_32_32
(suisho5), and KP256 (suishopetite) are mutually incompatible architectures.
Loading the wrong one triggers `exit(1)` during `isready`.

## Two-file eval

Unlike every other supported network, this one needs **two** files. The
layer-stack bucket is derived from progress coefficients that NAGISA_V4
ships in a separate `progress.bin` rather than inside `nn.bin`.

- Loader: pass `progressBin` alongside `evalBin`. Both land in `/eval`.
- Engine: `LS_PROGRESS_COEFF` names the file, resolved against `EvalDir`,
  defaulting to `progress.bin`.
- Without it the bucket cannot be computed and `isready` fails.
- `FV_SCALE` defaults to 28 on SFNN builds (16 elsewhere), matching the
  `eval_options.txt` shipped with the network. No manual setting needed.

## Bucket rule (`LS_BUCKET_MODE`)

| Value | Rule |
|---|---|
| `progress8ek` (default) | Mutual-entering-king positions map to the 9th stack (index 8). |
| `progress8kpabs` | Progress only, indices 0–7. The 9th layer stack is never selected. |

The default matches the `eval_options.txt` shipped with NAGISA_V4, which
specifies `progress8ek` (NAGISA_V3 defaulted to `progress8kpabs`). The two
rules diverge only in entering-king positions. **Both modes pass the eval
hash check**, so picking the wrong one silently selects different weights
with no warning.

## Memory Budget (4 GB max heap)

The initial heap is 512 MB rather than the 256 MB used by the other pthread
variants, because `nn.bin` alone is ~172 MB (180,556,853 bytes).
`progress.bin` adds ~1 MB (1,003,104 bytes).
