# yaneuraou-wasm-node-ka4096 — internal spec for LLM consumers

Sibling spec: `yaneuraou-wasm-node-kp256/SPEC.md` — read that for the
shared init sequence, loader contract, and behavioral notes. This file
only documents what differs.

## Package Identity

- npm scope: `@ultemica/yaneuraou-wasm-node-ka4096`
- Engine: YaneuraOu SFNN ka2 4096x2-8-64 (つようら王11 mini), multi-threaded
- Runtime: Node.js 18+ (`node:worker_threads`)
- License: GPL-3.0

## Wasm binary

- `YANEURAOU_EDITION=YANEURAOU_ENGINE_SFNN_ka2_4096_8_64_progress8x`
- `EM_EXPORT_NAME=YaneuraOu_Tuyoura11Mini`
- `EM_INITIAL_MEMORY_SIZE=268435456` (256 MiB)
- `EM_MAXIMUM_MEMORY_SIZE=4294967296` (4 GiB)
- `EM_STACK_SIZE=8388608` (8 MiB per thread)
- `EM_PTHREAD_POOL_SIZE=32` (workflow matrix)
- `EM_ENVIRONMENT=node`, `EM_PTHREAD=1`
- `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall','callMain']`
- `EM_CLOSURE=0`, Emscripten 3.1.43
- Other loader and runtime conventions follow `yaneuraou-wasm-node-kp256`;
  the matrix above overrides its memory and stack profile.

## Eval-format compatibility

SFNN ka2 (this), HalfKP_768x2_16_64 (aoba), HalfKP_256x2_32_32
(suisho5), and KP256 (suishopetite) are mutually incompatible architectures.
Loading the wrong one triggers `exit(1)` during `isready`.

## Two-file eval

This network needs **two** files. The
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

## Memory Budget (4 GiB configured max linear memory)

- Initial linear-memory capacity: 256 MiB; maximum: 4 GiB.
- Per-thread stack: 8 MiB (measured minimum 4.5 MiB: with `STACK_OVERFLOW_CHECK=2` the search aborts
  below it, at `go depth 14` from the start position). The worker pool is 32; the loader defaults to
  `Threads=1`. Pool size is not a recommendation to activate all workers.
- `nn.bin`: 7,711,372 bytes; `progress.bin`: 1,003,104 bytes;
  combined: 8,714,476 bytes.
- MEMFS files, decoded NNUE parameters, temporary loading copies, histories,
  optional books, and the configured hash all consume linear memory.
  JavaScript-side buffers are additional host memory. Peak usage and
  remaining headroom have not been measured.
