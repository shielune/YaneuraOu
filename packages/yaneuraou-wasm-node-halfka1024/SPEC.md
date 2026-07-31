# yaneuraou-wasm-node-nagisa — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-nagisa`
- Engine: YaneuraOu SFNN HalfKA_hm2 1024x2-15-64 (NAGISA_V3) V9.60, multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_SFNN_halfkahm2_1024_15_64_progress8ek`
- `EM_EXPORT_NAME=YaneuraOu_Nagisa`
- `EM_INITIAL_MEMORY_SIZE=268435456` (256 MB)
- `EM_MAXIMUM_MEMORY_SIZE=2147483648` (2 GB)
- Everything else identical to `yaneuraou-wasm-node-kp256`.

## Eval-format compatibility

SFNN HalfKA_hm2 (this), HalfKP_768x2_16_64 (aoba), HalfKP_256x2_32_32
(suisho5), and KP256 (suishopetite) are mutually incompatible architectures.
Loading the wrong one triggers `exit(1)` during `isready`.

## Two-file eval

Unlike every other supported network, this one needs **two** files. The
layer-stack bucket is derived from progress coefficients that NAGISA_V3
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
| `progress8kpabs` (default) | Progress only, indices 0–7. The 9th layer stack is never selected. |
| `progress8ek` | Mutual-entering-king positions map to the 9th stack (index 8). |

The default matches NAGISA_V3's own default. The two rules diverge only in
entering-king positions; both were verified to produce identical node counts
and moves on native (aarch64) and WASM.

## Memory Budget (2 GB max heap)

Same as `yaneuraou-wasm-node-halfkp256` — both pthread variants share the
256 MB / 2 GB profile.
