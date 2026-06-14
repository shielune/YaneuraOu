# @ultemica/yaneuraou-wasm-node-mobility

YaneuraOu Mobility / KIKI (164 次元学習済み線形評価) 将棋エンジンを
**Node.js から** multi-thread (pthread) で動かすための WASM パッケージ。

- エンジン: YaneuraOu Mobility/KIKI (V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: 128 MB initial / 1 GB max / 2 MB stack
- 評価関数: **内蔵** (`mobility_weights_embedded.cpp` に焼き込み済み)
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Sibling packages

| | `yaneuraou-wasm-mobility-cfworkers` | `yaneuraou-wasm-node-mobility` (this) |
|---|---|---|
| Target | Cloudflare Workers / V8 Isolate | Node.js 18+ |
| Threads | 1 (固定) | 可変 (1-32) |
| WASM heap | 64 MB / 128 MB max | 128 MB / 1 GB max |
| emscripten | 5.0.5 | **3.1.43** |

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-mobility";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-mobility/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve(
  "@ultemica/yaneuraou-wasm-node-mobility/wasm",
);
const wasmBinary = await fs.readFile(wasmPath);

// evalBin 不要 — mobility 重みはバイナリに内蔵されている。
const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
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

## Mobility-specific notes

- 評価関数は学習済み 164 次元線形 (mobility / KIKI 系特徴)。
- 強さは material 以上 NNUE 未満。NNUE が積めない環境で「やや強い軽量エンジン」が
  欲しいケース向け。
- 外部 `nn.bin` は不要 — `evalBin` を渡しても無視される。

## API

`yaneuraou-wasm-node-kp256` と同じ surface。

## Files in this package

`yaneuraou-wasm-node-material` と同じ構成。

## License

GPL-3.0 (inherited from YaneuraOu)
