# @ultemica/yaneuraou-wasm-pthread-halfkp128

YaneuraOu NNUE HalfKP128 将棋エンジンを multi-thread (pthread) で動かすための
ブラウザ向け WASM パッケージ。

- エンジン: YaneuraOu NNUE HalfKP128 (V8.50)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: 128MB initial / 4GB max / 2MB stack
- **`SharedArrayBuffer` を要求 — ホストは COOP/COEP ヘッダを返す必要**

## Sibling packages

| | `yaneuraou-wasm-cfworkers` | `yaneuraou-wasm-pthread-halfkp128` (this) |
|---|---|---|
| Threads | 1 (固定) | 可変 (1-32) |
| `SharedArrayBuffer` | not used | required |
| COOP/COEP headers | not needed | **required** |
| Cloudflare Workers | works | not supported |
| WASM heap | 64MB / 128MB max | 128MB / 4GB max |
| Recommended hash | 16MB | 64MB+ |

## Cross-origin isolation requirement

ブラウザは以下のヘッダが両方付いたページでのみ `SharedArrayBuffer` を有効化する:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

ヘッダを付けられないホスト (素の GitHub Pages 等) では `yaneuraou-wasm-cfworkers`
を使うこと。

## Usage (browser)

```ts
import { createEngine } from "@ultemica/yaneuraou-wasm-pthread-halfkp128";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-pthread-halfkp128/engine";
import wasmUrl from "@ultemica/yaneuraou-wasm-pthread-halfkp128/wasm?url";

const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
const evalBin = await fetch("/eval/nn.bin").then((r) => r.arrayBuffer());

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  evalBin,
  threads: navigator.hardwareConcurrency ?? 4,
  usiHash: 64,
});

const result = await engine.eval({
  sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
  byoyomi: 1_000,
});
console.log(result.bestmove, result.score);
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
利用者側で配信して `createEngine` に `ArrayBuffer` で渡す。

| File | Architecture | Size | Note |
|---|---|---|---|
| HalfKP_128x2_32_32 nn.bin | HalfKP128 | ~31 MB | 本パッケージに対応する評価関数 |
| 100T-shock book | - | 4.7 MB | 定跡 (任意) |
| 700T-shock book | - | 32 MB | 定跡 (大、HASH と合わせてヒープ要確認) |

> KP256 eval (~1 MB) は HalfKP128 エンジンと互換性なし。

## Building locally

```sh
bun install
bun run build:wasm     # invokes act to run the workflow
bun run build:loader   # tsc → dist/index.{js,d.ts}
```

## See also

- [`@ultemica/yaneuraou-wasm-cfworkers`](https://github.com/tsshogi/YaneuraOu) — single-thread variant for Cloudflare Workers
- [`@ultemica/yaneuraou-wasm-mate-pthread`](https://github.com/tsshogi/YaneuraOu) — multi-thread mate (tsume) solver

## License

GPL-3.0 (inherited from YaneuraOu)
