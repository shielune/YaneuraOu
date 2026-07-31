# @ultemica/yaneuraou-wasm-node-halfkp256

YaneuraOu NNUE HalfKP_256x2_32_32 (Suisho5 アーキ) 将棋エンジンを
**Node.js から** multi-thread (pthread) で動かすための WASM パッケージ。

- エンジン: YaneuraOu NNUE HalfKP_256x2_32_32 (Suisho5, V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: **256 MB initial / 4 GB max** / 2 MB stack
- 評価関数: 約 62 MB の `nn.bin` を実行時に渡す
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Sibling packages

| | `yaneuraou-wasm-pthread-halfkp256` | `yaneuraou-wasm-node-halfkp256` (this) |
|---|---|---|
| Target | Browser | Node.js 18+ |
| WASM heap | 256 MB / 4 GB max | 256 MB / 4 GB max |
| Cloudflare Workers | not provided (eval は cfworkers heap に乗らない) | n/a |
| emscripten | 5.0.5 | **3.1.43** |

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-halfkp256";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-halfkp256/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve(
  "@ultemica/yaneuraou-wasm-node-halfkp256/wasm",
);
const wasmBinary = await fs.readFile(wasmPath);
const evalBin = await fs.readFile("./eval/nn.bin"); // suisho5 HalfKP

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  evalBin,
  threads: 4,
  usiHash: 128,
});

const result = await engine.eval({
  sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
  byoyomi: 2_000,
});
console.log(result.bestmove, result.score);
engine.dispose();
```

## Eval files

`nn.bin` は HalfKP 専用フォーマット。KP256 のものとは互換性なし。

| File | Size | Source |
|---|---|---|
| suisho5 nn.bin | ~62 MB | https://github.com/mizar/YaneuraOu/releases/download/resource/suisho5_20211123.halfkp.nnue.cpp.gz |

`.cpp.gz` を解凍 → `script/eval_bin_to_cpp_literal.py` の逆変換で `.bin` に戻す。

## API

`yaneuraou-wasm-node-kp256` と同じ surface (`createEngine` / `Engine`)。

## Files in this package

- `dist/index.js` / `dist/index.d.ts` — the TypeScript loader
- `dist/yaneuraou.js` — the emscripten factory (ES module)
- `dist/yaneuraou.wasm` — the WASM binary (約 2 MB)
- `dist/yaneuraou.worker.js` — the classic pthread worker script (3.1.43-only)

## License

GPL-3.0 (inherited from YaneuraOu)
