# @ultemica/yaneuraou-wasm-node-kp256

YaneuraOu NNUE KP256 将棋エンジンを **Node.js から** multi-thread (pthread) で
動かすための WASM パッケージ。

- エンジン: YaneuraOu NNUE KP256 (V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: 128 MB initial / 1 GB max / 2 MB stack
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン
  (3.1.44–3.1.73 は ESM worker で stall、3.1.74+ は INCOMING_MODULE_JS_API
  回りで Node が止まる。`docs/wasm_client_usage.md` 参照)

## Sibling packages

| | `yaneuraou-wasm-kp256-cfworkers` | `yaneuraou-wasm-pthread-kp256` | `yaneuraou-wasm-node-kp256` (this) |
|---|---|---|---|
| Target | Cloudflare Workers / V8 Isolate | Browser | Node.js 18+ |
| Threads | 1 (固定) | 可変 (1-32) | 可変 (1-32) |
| `SharedArrayBuffer` | not used | required | uses worker_threads natively |
| COOP/COEP headers | not needed | required | n/a |
| WASM heap | 64 MB / 128 MB max | 128 MB / 1 GB max | 128 MB / 1 GB max |
| emscripten | 5.0.5 | 5.0.5 | **3.1.43** |

## Why Node-specific package?

Browser pthread builds (`EM_ENVIRONMENT=web,worker`) need a worker polyfill
to run under Node. This package ships a binary built with
`EM_ENVIRONMENT=node` and `EM_EXPORTED_RUNTIME_METHODS=['FS','ccall','callMain']`,
so emscripten emits a Node-native pthread runtime that drives
`node:worker_threads` directly. The price is that the binary won't load in
browsers — use the `pthread-kp256` package there.

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-kp256";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-kp256/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve("@ultemica/yaneuraou-wasm-node-kp256/wasm");
const wasmBinary = await fs.readFile(wasmPath);
const evalBin = await fs.readFile("./eval/nn.bin");

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  evalBin,
  threads: 4,
  usiHash: 64,
});

const result = await engine.eval({
  sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
  byoyomi: 1_000,
});
console.log(result.bestmove, result.score);
engine.dispose();
```

## API

```ts
createEngine(opts: CreateEngineOptions): Promise<Engine>;

interface Engine {
  send(command: string): void;
  eval(req: EvalRequest): Promise<EvalResult>;
  evalBatch(reqs: EvalRequest[]): Promise<EvalResult[]>;
  dispose(): void;
}

interface EvalRequest {
  sfen: string;
  byoyomi?: number;
  skillLevel?: number;
  multiPv?: number;
  depthLimit?: number;
  nodesLimit?: number;
}

interface EvalResult {
  bestmove: string;
  ponder: string | null;
  score: { kind: "cp" | "mate"; value: number; bound?: "lowerbound" | "upperbound" } | null;
  depth: number | null;
  seldepth: number | null;
  nodes: number | null;
  timeMs: number | null;
  pv: string[] | null;
  lastInfo: string | null;
  bestmoveLine: string;
}
```

## Eval / book files

NNUE 評価関数 (`nn.bin`) と定跡ファイルはバイナリに同梱しない。
利用者側で取得して `createEngine` に `Uint8Array` または `ArrayBuffer` で渡す。

| File | Architecture | Size | Source |
|---|---|---|---|
| suishopetite nn.bin | KP256 | ~873 KB | https://github.com/mizar/YaneuraOu/releases/download/resource/suishopetite_20211123.k_p.nnue.cpp.gz |
| 100T-shock book | - | 4.7 MB | https://github.com/yaneurao/YaneuraOu/releases/download/BOOK-100T-Shock/100T-shock-book.zip |
| 700T-shock book | - | 32 MB | https://github.com/yaneurao/YaneuraOu/releases/download/BOOK-700T-Shock/700T-shock-book.zip |

> suishopetite は embedded C++ array 形式の `.cpp.gz`。
> `script/eval_bin_to_cpp_literal.py` の逆変換で `.bin` に戻す。
> HalfKP eval (62 MB) は KP256 エンジンと互換性なし。

## Files in this package

- `dist/index.js` / `dist/index.d.ts` — the TypeScript loader (`createEngine`)
- `dist/yaneuraou.js` — the emscripten factory (ES module)
- `dist/yaneuraou.wasm` — the WASM binary
- `dist/yaneuraou.worker.js` — the classic pthread worker script (3.1.43-only;
  emscripten reads this from the engine's directory at runtime via
  `locateFile`. Do not remove.)

## License

GPL-3.0 (inherited from YaneuraOu)
