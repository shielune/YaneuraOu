# @ultemica/yaneuraou-wasm-node-nagisa

YaneuraOu NNUE NAGISA V3.1 SFNN HalfKA_hm2_1024x2_15_64 (NAGISA V3.1 アーキ) 将棋エンジンを
**Node.js から** multi-thread (pthread) で動かすための WASM パッケージ。

- エンジン: YaneuraOu NNUE NAGISA V3.1 SFNN HalfKA_hm2_1024x2_15_64 (NAGISA V3.1 形, V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: **256 MB initial / 2 GB max** / 2 MB stack
- 評価関数: 数十 MB の `nn.bin` を実行時に渡す
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-nagisa";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-nagisa/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve(
  "@ultemica/yaneuraou-wasm-node-nagisa/wasm",
);
const wasmBinary = await fs.readFile(wasmPath);
const evalBin = await fs.readFile("./eval/nn.bin"); // NAGISA V3.1 HalfKP_768

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

| File | Size | Source |
|---|---|---|
| NAGISA V3.1 eval files | 数十 MB | https://github.com/keinoda/YaneuraOu/releases/tag/nagisa-v3.1 |

> NAGISA V3.1 は素の `nn.bin` がそのまま配布されている (解凍不要)。

> NAGISA V3.1 SFNN HalfKA_hm2_1024x2_15_64 は HalfKP_256x2_32_32 (Suisho5) と互換性なし。
> KP256 とも互換性なし。

## API

`yaneuraou-wasm-node-kp256` と同じ surface。

## Files in this package

`yaneuraou-wasm-node-halfkp256` と同じ構成。

## License

GPL-3.0 (inherited from YaneuraOu)
