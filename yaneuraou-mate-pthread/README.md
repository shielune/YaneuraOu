# @ultemica/yaneuraou-mate-pthread

YaneuraOu **mate (tsume) solver** as a multi-threaded WASM bundle for browsers.

- DfPn 詰将棋 solver based on `YANEURAOU_MATE_ENGINE`
- Multi-thread (`EM_PTHREAD=1`) — uses `SharedArrayBuffer`, **requires COOP/COEP**
- Compared to `yaneuraou-mate-cfworkers`: `go mate <ms>` time limits are honored
- 128MB initial / 1GB max memory; 1MB hash is enough for typical practical-game tsume

## When to use which package

| | `yaneuraou-mate-cfworkers` | `yaneuraou-mate-pthread` (this) |
|---|---|---|
| Threads | single | multi (pthread) |
| `SharedArrayBuffer` | not used | required |
| COOP/COEP headers | not needed | **required** |
| Cloudflare Workers | works | not supported |
| `go mate <ms>` time limit | ignored | honored |
| Static / shared hosting | works on any host | host must serve cross-origin isolation headers |

## Cross-origin isolation requirement

Browsers only expose `SharedArrayBuffer` to pages served with both:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

If your host cannot set these (e.g. plain GitHub Pages, generic CDN without
custom headers), use `yaneuraou-mate-cfworkers` instead.

## Usage (browser)

```ts
import { createMateEngine } from "@ultemica/yaneuraou-mate-pthread";
import YaneuraOuFactory from "@ultemica/yaneuraou-mate-pthread/engine";
import wasmUrl from "@ultemica/yaneuraou-mate-pthread/wasm?url";

const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
const engine = await createMateEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  usiHash: 1,
  threads: 1,
});

const result = await engine.solve({
  sfen: "ln1gkg1nl/6+P2/2sppps1p/2p3p2/p8/P1P1P3P/2NP1PP2/3s1KSR1/L1+b2G1NL w R2Pbgp 42",
  byoyomi: 5_000,
  nodesLimit: 1_000_000,
});

if (result.status === "mate") {
  console.log("詰み:", result.moves.join(" "));
} else if (result.status === "timeout") {
  console.log("時間切れ");
} else {
  console.log("不詰:", result.status);
}
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
  byoyomi?: number;     // ms; honored in pthread build
  nodesLimit?: number;  // 0 = unlimited
}

interface SolveResult {
  status: "mate" | "nomate" | "timeout" | "none";
  moves: string[];
  nodes: number | null;
  timeMs: number | null;
  wallMs: number;
  lastInfo: string | null;
}
```

## Limitations vs. native YaneuraOu mate engine

- **DfPn search itself is not parallelised.** Setting `threads > 1` does not
  speed up the search; the extra worker thread only keeps the JS main thread
  responsive while DfPn runs.
- **DfPn finds A forced mate, not always the shortest.** For shortest-mate
  guarantees, run iterative deepening above this engine.

## Building locally

```sh
bun install
bun run build:wasm    # invokes act to run the workflow
bun run build:loader  # tsc → dist/index.{js,d.ts}
```

## See also

- [`@ultemica/yaneuraou-mate-cfworkers`](https://github.com/tsshogi/YaneuraOu) — single-thread variant for Cloudflare Workers and headers-restricted browsers
- [`@ultemica/yaneuraou-cfworkers`](https://github.com/tsshogi/YaneuraOu) — NNUE play engine (single-thread)

## License

GPL-3.0 (inherited from YaneuraOu)
