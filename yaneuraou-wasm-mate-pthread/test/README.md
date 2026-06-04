# yaneuraou-wasm-mate-pthread / test

End-to-end smoke test for the multi-threaded mate WASM build. Verifies that:

- The browser bundle loads under cross-origin isolation (COOP/COEP).
- `SharedArrayBuffer` is available, pthread workers spawn.
- A 3-ply mate problem is solved via `createMateEngine()` and returns
  `status === "mate"` with a 3-move sequence.

## Prerequisites

- WASM artifacts present in `../dist/yaneuraou.{js,wasm}` (run the build
  workflow or the `make tournament … EM_PTHREAD=1` invocation locally).
- TypeScript loader compiled to `../dist/index.js` (`bun tsc`).
- `playwright` installed and Chromium downloaded:
  `bunx --bun playwright install chromium`.

## Run

```sh
cd yaneuraou-wasm-mate-pthread
bun run test/run.ts
```

The script starts `test/server.ts` on a random port (with COOP/COEP headers),
launches headless Chromium via Playwright, opens `test/index.html`, and
exercises the engine through `window.runMateTest()`. Exit code is non-zero
on any failure.
