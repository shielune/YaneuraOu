# @ultemica/yaneuraou-wasm-pthread-nagisa

YaneuraOu SFNN HalfKA_hm2 将棋エンジンを multi-thread (pthread) で動かすための
ブラウザ向け WASM パッケージ。

- エンジン: YaneuraOu SFNN `HalfKA_hm2 1024x2-15-64` / LayerStack 9 (NAGISA_V3 互換)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: 256MB initial / 4GB max / 2MB stack
- 評価関数は同梱せず外部ロード ([NAGISA_V3 Release](https://github.com/keinoda/YaneuraOu/releases/tag/nagisa-v3.1) の `nn.bin` と `progress.bin`)
- **`SharedArrayBuffer` を要求 — ホストは COOP/COEP ヘッダを返す必要**

## Sibling packages

| | `yaneuraou-wasm-cfworkers` | `yaneuraou-wasm-pthread-nagisa` (this) |
|---|---|---|
| Engine | NNUE KP256 (Suisho petite) | SFNN HalfKA_hm2 (NAGISA_V3) |
| Threads | 1 (固定) | 可変 (1-32) |
| `SharedArrayBuffer` | not used | required |
| COOP/COEP headers | not needed | **required** |
| Cloudflare Workers | works | not supported |
| WASM heap | 64MB / 128MB max | 256MB / 4GB max |
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

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  evalBin,
  progressBin,
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

評価関数 (`nn.bin` / `progress.bin`) と定跡ファイルはバイナリに同梱しない。
利用者側で配信して `createEngine` に `ArrayBuffer` で渡す。

| File | Architecture | Size | Note |
|---|---|---|---|
| NAGISA_V3 nn.bin | SFNN HalfKA_hm2 1024x2-15-64 | ~75 MB | [NAGISA_V3 Release](https://github.com/keinoda/YaneuraOu/releases/tag/nagisa-v3.1) のアーカイブに同梱 |
| NAGISA_V3 progress.bin | (同上) | ~1 MB | **必須。** LayerStack 選択に使う進行度係数 |
| 100T-shock book | - | 4.7 MB | 定跡 (任意) |
| 700T-shock book | - | 32 MB | 定跡 (大、HASH と合わせてヒープ要確認) |

> **この評価関数は 2 ファイル必要。** 他のネットワークと違い進行度係数が
> `nn.bin` ではなく別ファイルの `progress.bin` にあり、渡さないと bucket を
> 計算できない。単体配布は無く、リリースページの各プラットフォーム版
> アーカイブに `eval/` として同梱されている (3 種のどれでも中身は同一)。

> `FV_SCALE` はこのビルドでは既定で **28** (他のネットワークは 16)。
> 配布物の `eval_options.txt` が指定している値と同じなので、手で設定する必要はない。

> Suisho5 由来の HalfKP256 eval や KP256 eval は本パッケージとは互換性なし。

### LayerStack の bucket 規則

`LS_BUCKET_MODE` USI option で切り替える。

| 値 | 規則 |
|---|---|
| `progress8kpabs` (既定) | 進行度のみで 0〜7 を選ぶ。9 個目の LayerStack は使わない |
| `progress8ek` | 相入玉局面を 9 個目 (index 8) に割り当てる |

既定は本家 NAGISA_V3 と同じ `progress8kpabs`。両者の結果が変わるのは
相入玉局面のみ。

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
