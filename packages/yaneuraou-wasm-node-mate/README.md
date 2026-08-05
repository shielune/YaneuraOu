# @ultemica/yaneuraou-wasm-node-mate

YaneuraOu 詰将棋ソルバ (DfPn 探索エンジン) を **Node.js から** multi-thread (pthread)
で動かすための WASM パッケージ。

- エンジン: YaneuraOu Mate (DfPn) (V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: 128 MB initial / 4 GB max / 2 MB stack
- 評価関数: **不要** (DfPn は eval を使わない)
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Sibling packages

| | `yaneuraou-wasm-mate-cfworkers` | `yaneuraou-wasm-mate-pthread` | `yaneuraou-wasm-node-mate` (this) |
|---|---|---|---|
| Target | Cloudflare Workers / V8 Isolate | Browser | Node.js 18+ |
| Threads | 1 (固定) | 可変 (1-32) | 可変 (1-32) |
| `SharedArrayBuffer` | not used | required | uses worker_threads natively |
| COOP/COEP headers | not needed | required | n/a |
| WASM heap | 64 MB / 128 MB max | 128 MB / 4 GB max | 128 MB / 4 GB max |
| emscripten | 5.0.5 | 5.0.5 | **3.1.43** |

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-mate";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-mate/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve("@ultemica/yaneuraou-wasm-node-mate/wasm");
const wasmBinary = await fs.readFile(wasmPath);

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  threads: 4,
  usiHash: 256,
});

// Mate solver: DfPn searches deeper with higher Hash. Use 'go mate' or
// just a deep `go movetime` and read the bestmove / score (mate value).
const result = await engine.eval({
  sfen: "...",        // 詰めたい局面
  byoyomi: 5_000,
  depthLimit: 0,
});
console.log(result.bestmove, result.score);
engine.dispose();
```

## Mate-specific notes

- 評価関数 (`nn.bin`) は不要。`evalBin` オプションは無視される。
- DfPn は `USI_Hash` が **大きいほど** 深く探索できる。256 MB 以上を推奨。
- `Threads` を増やすと並列探索になるが、DfPn の並列効率は NNUE 評価探索ほど
  伸びない (約 1.5-2x @ 4 threads)。

## API

`yaneuraou-wasm-node-kp256` と同じ surface (`createEngine` / `Engine` /
`EvalRequest` / `EvalResult`)。

## Files in this package

- `dist/index.js` / `dist/index.d.ts` — the TypeScript loader (`createEngine`)
- `dist/yaneuraou.js` — the emscripten factory (ES module)
- `dist/yaneuraou.wasm` — the WASM binary
- `dist/yaneuraou.worker.js` — the classic pthread worker script (3.1.43-only)

## License

GPL-3.0 (inherited from YaneuraOu)
