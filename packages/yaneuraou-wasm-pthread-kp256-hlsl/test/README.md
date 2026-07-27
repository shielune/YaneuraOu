# yaneuraou-wasm-pthread-kp256 / test

Smoke test for the multi-threaded NNUE_KP256 WASM build. Verifies that:

- The browser bundle loads under cross-origin isolation (COOP/COEP).
- `SharedArrayBuffer` is available, pthread workers spawn.
- The engine completes the USI handshake (`usi` -> `usiok`) and emits an
  `id name` line that mentions `YaneuraOu`.

Eval-dependent behaviour (`isready`, `go movetime`) is not exercised here —
that requires a real `nn.bin` and belongs to the consuming application.

## Prerequisites

- WASM artifacts present in `../dist/yaneuraou.{js,wasm}` (run the build
  workflow or the `make tournament … EM_PTHREAD=1` invocation locally).
- `playwright` installed and Chromium downloaded:
  `bunx --bun playwright install chromium`.

## Run

```sh
cd yaneuraou-wasm-pthread-kp256
bun run test/run.ts
```
