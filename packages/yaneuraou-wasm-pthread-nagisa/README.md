# @ultemica/yaneuraou-wasm-pthread-nagisa

YaneuraOu NNUE HalfKP768 将棋エンジンを multi-thread (pthread) で動かすための
ブラウザ向け WASM パッケージ。

- エンジン: YaneuraOu NNUE `NAGISA V3.1 SFNN HalfKA_hm2_1024x2_15_64` (NAGISA V3.1 互換ネットワーク構造)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: 256MB initial / 2GB max / 2MB stack
- 評価関数は同梱せず外部ロード ([NAGISA V3.1 Release](https://github.com/keinoda/YaneuraOu/releases/tag/nagisa-v3.1) の `nn.bin`、`progress.bin`、`eval_options.txt` を想定)
- **`SharedArrayBuffer` を要求 — ホストは COOP/COEP ヘッダを返す必要**

## Sibling packages

| | `yaneuraou-wasm-cfworkers` | `yaneuraou-wasm-pthread-nagisa` (this) |
|---|---|---|
| Engine | NNUE KP256 (Suisho petite) | NNUE HalfKP768 (NAGISA V3.1) |
| Threads | 1 (固定) | 可変 (1-32) |
| `SharedArrayBuffer` | not used | required |
| COOP/COEP headers | not needed | **required** |
| Cloudflare Workers | works | not supported |
| WASM heap | 64MB / 128MB max | 256MB / 2GB max |
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
import { createEngine } from "@ultemica/yaneuraou-wasm-pthread-nagisa";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-pthread-nagisa/engine";
import wasmUrl from "@ultemica/yaneuraou-wasm-pthread-nagisa/wasm?url";

const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
const evalBin = await fetch("/eval/nn.bin").then((r) => r.arrayBuffer());
const progressBin = await fetch("/eval/progress.bin").then((r) => r.arrayBuffer());
const evalOptions = await fetch("/eval/eval_options.txt").then((r) => r.arrayBuffer());

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  evalBin,
  evalFiles: [
    { name: "progress.bin", data: progressBin },
    { name: "eval_options.txt", data: evalOptions },
  ],
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

NAGISA V3.1 の評価ファイル (`nn.bin`、`progress.bin`、`eval_options.txt`) と定跡ファイルはバイナリに同梱しない。
利用者側で配信して `createEngine` に `ArrayBuffer` で渡す。

| File | Architecture | Size | Note |
|---|---|---|---|
| NAGISA V3.1 eval files | NAGISA V3.1 SFNN HalfKA_hm2_1024x2_15_64 | 数十 MB | [NAGISA V3.1 Release](https://github.com/keinoda/YaneuraOu/releases/tag/nagisa-v3.1) から `nn.bin`、`progress.bin`、`eval_options.txt` をすべて取得。本パッケージに対応する唯一の評価関数 |
| 100T-shock book | - | 4.7 MB | 定跡 (任意) |
| 700T-shock book | - | 32 MB | 定跡 (大、HASH と合わせてヒープ要確認) |

> Suisho5 由来の HalfKP256 eval (~62 MB) や KP256 eval (~1 MB) は本パッケージとは互換性なし。
> 同じ HalfKP 入力特徴量でも `768x2-16-64` と `256x2-32-32` はネットワーク構造が異なる。

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
