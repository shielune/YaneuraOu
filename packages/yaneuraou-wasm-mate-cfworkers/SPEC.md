# yaneuraou-wasm-mate-cfworkers — internal spec for LLM consumers

This document describes the internal contract between the Wasm binary, the
TypeScript loader, and the consumer code. It is intended for code-generating
LLMs and humans extending this package.

## Wasm binary

- Source: `source/engine/yaneuraou-mate-engine/yaneuraou-mate-search.cpp`
- Build edition: `YANEURAOU_EDITION=YANEURAOU_MATE_ENGINE`
- Linker flags (em++):
  - `EM_EXPORT_NAME=YaneuraOu_MATE`
  - `EM_ENVIRONMENT=web`
  - `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall','HEAPU8']`
  - `EM_PTHREAD=0`
  - `EM_INITIAL_MEMORY_SIZE=67108864` (64 MB)
  - `EM_MAXIMUM_MEMORY_SIZE=134217728` (128 MB)
  - `EM_STACK_SIZE=2097152` (2 MB)
  - `EM_CLOSURE=0`
- Emscripten: 5.0.5 (docker image `emscripten/emsdk:5.0.5`)

## Single-thread variant

`source/engine/yaneuraou-mate-engine/yaneuraou-mate-search.cpp` defines
`MainThread::search()` twice — once guarded by
`#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)` (the
single-thread variant used here) and once for normal pthread builds.

The single-thread variant runs `solver.mate_dfpn(rootPos, NodesLimit)`
synchronously in the engine's main thread. Because the host JS event loop
is blocked while DfPn runs, **`go mate <ms>` time limits are not enforced**
— termination is by `NodesLimit` or proof completion.

## USI surface

The engine accepts a subset of standard USI:

- `usi` → emits `id name … / id author … / option … / usiok`
- `setoption name <X> value <Y>` (no response)
  - Recognised names: `Threads`, `USI_Hash`, `WriteDebugLog`,
    `GenerateAllLegalMoves`, `PvInterval`, `SolverType`, `NodesLimit`
- `isready` → emits `info string DfPn memory allocation , USI_Hash = … [MB]`
  followed by `readyok`
- `position sfen <SFEN>` (no response)
- `go mate <ms>` / `go mate 0` / `go mate infinite` — runs DfPn
  - emits `info time <ms> nodes <N> nps <X> hashfull <H> pv <PV>`
  - emits one of:
    - `checkmate <move> <move> …` — proven mate, USI moves
    - `checkmate nomate` — proven no mate
    - `checkmate timeout` — only in pthread builds
    - `checkmate none` — search aborted by NodesLimit / OOM
- `quit` → terminates

## JS bridge (`wasm_pre.js`)

The Wasm module is wrapped by `source/wasm_pre.js` (linked via `--pre-js`).
It overrides `Module["print"]` so output is delivered to listeners instead
of `console.log`, and exposes:

```ts
instance.postMessage(command: string): void;        // queue USI command
instance.addMessageListener(fn: (line) => void);    // subscribe
instance.removeMessageListener(fn);
instance.terminate();                               // stop processing queue
```

`postMessage` enqueues the command; an internal poll loop drains it via
`Module["ccall"]("usi_command", "number", ["string"], [cmd])`. If `ccall`
returns a non-zero "tryLater", the command is requeued and the poll
back-off doubles.

Output lines are dispatched to listeners on a `setTimeout(0)` to break
the engine's call stack. Listeners receive raw USI lines.

## TypeScript loader (`src/index.ts`)

`createMateEngine(opts)` returns an object with:

```ts
send(command: string): void;
solve(req: SolveRequest): Promise<SolveResult>;
dispose(): void;
```

`solve()` is serialised through an internal `chain` promise so concurrent
calls cannot interleave. Each `solve()`:

1. Issues `setoption name NodesLimit value <N>`.
2. Issues `position sfen <SFEN>`.
3. Records `lines.length` as `goMark`.
4. Issues `go mate <ms>` (or `go mate 0`).
5. Polls until a `checkmate` line appears in `lines.slice(goMark)`.
6. Parses the slice into a `SolveResult` and clears `lines`.

There is no `usinewgame` between solves — the DfPn hash table is reused.

## Memory expectations

- Engine boot: 64 MB heap (the WASM `INITIAL_MEMORY`).
- After `isready` with `USI_Hash=1MB`: heap stays at 64 MB (the 1 MB hash
  fits in unused initial memory).
- After `isready` with `USI_Hash=64MB`: heap grows to ~77 MB (12.8 MB
  engine static + 64 MB hash). `EM_MAXIMUM_MEMORY_SIZE` caps growth at
  128 MB.
- DfPn typically uses < 6 KB per node, so 1 MB hash supports thousands of
  proof-tree entries — enough for practical tsume (3 / 5 / 7 / 9 / 11 ply).

## Performance expectations (ARM64 devcontainer, single thread)

| Problem | DfPn nodes | Engine CPU | Wall (postMessage RTT included) |
|---|---|---|---|
| 3-ply mate | 4-21 | 0-2 ms | 1.7-4.6 ms |
| 7-ply mate | 46-360 | 0-1 ms | 1.7-2.8 ms |
| 11-ply mate | 113-5525 | 0-5 ms | 1.9-6.9 ms |

Wall time on production Workers / browsers is typically dominated by
network and JS event-loop overhead, not the WASM search itself.
