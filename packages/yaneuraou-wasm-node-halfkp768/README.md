# @ultemica/yaneuraou-wasm-node-halfkp768

YaneuraOu NNUE HalfKP_768x2_16_64 (AobaNNUE アーキ) 将棋エンジンを
**Node.js から** multi-thread (pthread) で動かすための WASM パッケージ。

- エンジン: YaneuraOu NNUE HalfKP_768x2_16_64 (AobaNNUE 形, V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: **256 MB initial / 4 GB max** / 2 MB stack
- 評価関数: ~184 MB の `nn.bin` を実行時に渡す
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-halfkp768";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-halfkp768/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve(
  "@ultemica/yaneuraou-wasm-node-halfkp768/wasm",
);
const wasmBinary = await fs.readFile(wasmPath);
const evalBin = await fs.readFile("./eval/nn.bin"); // AobaNNUE HalfKP_768

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
| AobaNNUE nn.bin | ~184 MB | https://github.com/yssaya/AobaNNUE/releases |

> AobaNNUE は素の `nn.bin` がそのまま配布されている (解凍不要)。

> HalfKP_768x2_16_64 は HalfKP_256x2_32_32 (Suisho5) と互換性なし。
> KP256 とも互換性なし。

## API

`yaneuraou-wasm-node-kp256` と同じ surface。

## Files in this package

`yaneuraou-wasm-node-halfkp256` と同じ構成。

## License

GPL-3.0 (inherited from YaneuraOu)
