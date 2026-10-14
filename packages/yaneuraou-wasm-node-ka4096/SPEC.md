# yaneuraou-wasm-node-ka4096 — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-ka4096`
- Engine: YaneuraOu SFNN ka2 4096x2-7-64 (つようら王11 mini) V9.60, multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_SFNN_ka2_4096_7_64_progress8x`
- `EM_EXPORT_NAME=YaneuraOu_Tuyoura11Mini`
- `EM_INITIAL_MEMORY_SIZE=268435456` (256 MB)
- `EM_MAXIMUM_MEMORY_SIZE=2147483648` (2 GB)
- Everything else identical to `yaneuraou-wasm-node-kp256`.

## Eval-format compatibility

SFNN ka2 (this), HalfKP_768x2_16_64 (aoba), HalfKP_256x2_32_32
(suisho5), and KP256 (suishopetite) are mutually incompatible architectures.
Loading the wrong one triggers `exit(1)` during `isready`.

## Two-file eval

Unlike every other supported network, this one needs **two** files. The
layer-stack bucket is derived from progress coefficients that つようら王11 mini
ships in a separate `progress.bin` rather than inside `nn.bin`.

- Loader: pass `progressBin` alongside `evalBin`. Both land in `/eval`.
- Engine: `LS_PROGRESS_COEFF` names the file, resolved against `EvalDir`,
  defaulting to `progress.bin`.
- Without it the bucket cannot be computed and `isready` fails.
- `FV_SCALE` defaults to 28 on SFNN builds, but this network expects 24. The loader's
  `fvScale` option (default 24) sends `setoption name FV_SCALE`.

## Bucket rule

Progress only, indices 0-7 (`progress8x`: no entering-king bucket). There is no
`LS_BUCKET_MODE` option on this edition.

## Memory Budget (2 GB max heap)

Same as `yaneuraou-wasm-node-halfkp256` — both pthread variants share the
256 MB / 2 GB profile.
