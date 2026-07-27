# @ultemica/yaneuraou-wasm-node-material

YaneuraOu Material Lv1 (手作り駒得評価) 将棋エンジンを
**Node.js から** multi-thread (pthread) で動かすための WASM パッケージ。

- エンジン: YaneuraOu Material Lv1 (handcrafted, V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: 128 MB initial / 1 GB max / 2 MB stack
- 評価関数: **内蔵** (handcrafted、外部ファイル不要)
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Sibling packages

| | `yaneuraou-wasm-material-cfworkers` | `yaneuraou-wasm-node-material` (this) |
|---|---|---|
| Target | Cloudflare Workers / V8 Isolate | Node.js 18+ |
| Threads | 1 (固定) | 可変 (1-32) |
| WASM heap | 64 MB / 128 MB max | 128 MB / 1 GB max |
| emscripten | 5.0.5 | **3.1.43** |

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-material";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-material/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve(
  "@ultemica/yaneuraou-wasm-node-material/wasm",
);
const wasmBinary = await fs.readFile(wasmPath);

// evalBin 不要 — material 評価関数はバイナリに内蔵されている。
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

## Material-specific notes

- 評価関数は **MATERIAL_LEVEL=1** (素朴な駒得のみ) で焼かれている。
- 外部 `nn.bin` は不要 — `evalBin` を渡しても無視される。
- 強さは NNUE (KP256 / HalfKP) より大幅に弱い。教育用 / 初級者向け対局 /
  軽量な eval が欲しいケース向け。

## API

`yaneuraou-wasm-node-kp256` と同じ surface。

## Files in this package

- `dist/index.js` / `dist/index.d.ts` — the TypeScript loader
- `dist/yaneuraou.js` — the emscripten factory (ES module)
- `dist/yaneuraou.wasm` — the WASM binary (material 評価込み)
- `dist/yaneuraou.worker.js` — the classic pthread worker script (3.1.43-only)

## License

GPL-3.0 (inherited from YaneuraOu)
