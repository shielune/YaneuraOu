# @ultemica/yaneuraou-mate-cfworkers

YaneuraOu **mate (tsume) solver** as a single-threaded WASM bundle, ready to drop into a Cloudflare Workers fetch handler or a browser.

- DfPn 詰将棋 solver based on `YANEURAOU_MATE_ENGINE`
- Single-thread (`EM_PTHREAD=0`) — **no `SharedArrayBuffer`, no COOP/COEP headers required**
- 64MB initial / 128MB max memory; 1MB hash is enough for typical practical-game tsume (~15 ply)
- No eval / book files — just position + `go mate`

## Why a separate package?

The flagship `@ultemica/yaneuraou-cfworkers` package ships the NNUE search engine for normal play. That engine cannot solve tsume. This package ships **only the mate solver** and is much smaller (~560KB wasm).

## Usage

### Browser (Vite / esbuild / etc.)

```ts
import { createMateEngine } from "@ultemica/yaneuraou-mate-cfworkers";
import YaneuraOuFactory from "@ultemica/yaneuraou-mate-cfworkers/engine";
import wasmUrl from "@ultemica/yaneuraou-mate-cfworkers/wasm?url";

const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
const engine = await createMateEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  usiHash: 1, // MB; 1MB is plenty
});

const result = await engine.solve({
  sfen: "ln1gkg1nl/6+P2/2sppps1p/2p3p2/p8/P1P1P3P/2NP1PP2/3s1KSR1/L1+b2G1NL w R2Pbgp 42",
  nodesLimit: 100_000,
});

if (result.status === "mate") {
  console.log("詰み:", result.moves.join(" "));
} else {
  console.log("不詰:", result.status);
}
```

### Cloudflare Workers

```ts
import { createMateEngine } from "@ultemica/yaneuraou-mate-cfworkers";
import YaneuraOuFactory from "@ultemica/yaneuraou-mate-cfworkers/engine";
import wasmBinary from "@ultemica/yaneuraou-mate-cfworkers/wasm";

export default {
  async fetch(req: Request): Promise<Response> {
    const { sfen } = await req.json();
    const engine = await createMateEngine({
      factory: YaneuraOuFactory,
      wasmBinary,
    });
    const result = await engine.solve({ sfen, nodesLimit: 100_000 });
    engine.dispose();
    return Response.json(result);
  },
};
```

## API

```ts
createMateEngine(opts: CreateMateEngineOptions): Promise<MateEngine>;

interface MateEngine {
  send(command: string): void;
  solve(req: SolveRequest): Promise<SolveResult>;
  dispose(): void;
}

interface SolveRequest {
  sfen: string;
  byoyomi?: number;     // ms; ignored in single-thread WASM
  nodesLimit?: number;  // 0 = unlimited
}

interface SolveResult {
  status: "mate" | "nomate" | "timeout" | "none";
  moves: string[];      // mating sequence, USI moves
  nodes: number | null;
  timeMs: number | null;
  wallMs: number;
  lastInfo: string | null;
}
```

## Limitations vs. native YaneuraOu mate engine

- **No `go mate <ms>` time limit.** With `EM_PTHREAD=0`, the search runs synchronously and ignores time. Use `nodesLimit` instead.
- **No PV streaming.** Only the final `checkmate` line is parsed.
- **DfPn finds A forced mate, not always the shortest.** For an 11-mate problem it may return a 15-move sequence (a longer forced path through the proof tree). If your application requires shortest-mate guarantees, run an iterative deepening search above this engine.

## Building locally

```sh
# requires emsdk locally OR docker + act
npm install
npm run build:wasm    # invokes act to run the workflow
npm run build:loader  # tsc → dist/index.{js,d.ts}
```

## See also

- [`@ultemica/yaneuraou-cfworkers`](https://github.com/tsshogi/YaneuraOu) — the NNUE play engine

## License

GPL-3.0 (inherited from YaneuraOu)
