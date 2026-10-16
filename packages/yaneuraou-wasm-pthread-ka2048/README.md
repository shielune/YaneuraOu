# @ultemica/yaneuraou-wasm-pthread-ka2048

YaneuraOu SFNN ka2 将棋エンジンを multi-thread (pthread) で動かすための
ブラウザ向け WASM パッケージ。

- エンジン: YaneuraOu SFNN `ka2 2048x2-8-64` / LayerStack 8 (つようら王11 micro 互換)
- スレッド: 可変 (`Threads` USI option、ビルドの worker pool は 8)
- WASM SIMD: 有効
- メモリ: 256 MiB initial / 4 GiB max / 16 MiB per-thread stack
- 評価関数は同梱せず外部ロード (やねうら王 FANBOX で MIT ライセンス公開された `nn.bin` と `progress.bin`)
- **`SharedArrayBuffer` を要求 — ホストは COOP/COEP ヘッダを返す必要**

## Sibling packages

| | `yaneuraou-wasm-cfworkers` | `yaneuraou-wasm-pthread-ka2048` (this) |
|---|---|---|
| Engine | NNUE KP256 (Suisho petite) | SFNN ka2 (つようら王11 micro) |
| Threads | 1 (固定) | 可変 (worker pool: 8) |
| `SharedArrayBuffer` | not used | required |
| COOP/COEP headers | not needed | **required** |
| Cloudflare Workers | works | not supported |
| WASM heap | 64MB / 128MB max | 256 MiB / 4 GiB max |
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
import { createEngine } from "@ultemica/yaneuraou-wasm-pthread-ka2048";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-pthread-ka2048/engine";
import wasmUrl from "@ultemica/yaneuraou-wasm-pthread-ka2048/wasm?url";

const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
const evalBin = await fetch("/eval/nn.bin").then((r) => r.arrayBuffer());
const progressBin = await fetch("/eval/progress.bin").then((r) => r.arrayBuffer());

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  evalBin,
  progressBin,
  threads: Math.min(8, navigator.hardwareConcurrency ?? 4),
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
| つようら王11 micro nn.bin | SFNN ka2 2048x2-8-64 | 3,864,610 bytes | やねうら王 FANBOX で MIT ライセンス公開された配布物 |
| つようら王11 micro progress.bin | (同上) | 1,003,104 bytes | **必須。** LayerStack 選択に使う進行度係数 |
| 100T-shock book | - | 4,877,948 bytes | 定跡 (任意) |
| 700T-shock book | - | 33,340,394 bytes | 定跡 (大、HASH と合わせてヒープ要確認) |

> **この評価関数は 2 ファイル必要。** 他のネットワークと違い進行度係数が
> `nn.bin` ではなく別ファイルの `progress.bin` にあり、渡さないと bucket を
> 計算できない。評価関数は [公式配布記事](https://yaneurao.fanbox.cc/posts/12761272) の
> micro 用 `.7z` から別途取得する。WASM パッケージには同梱しない。

> `FV_SCALE` は **24** を指定すること。このビルドの既定は 28 だが、つようら王11 は 24 前提。
> `createEngine` の `fvScale` オプション (既定 24) が `setoption name FV_SCALE` を送る。

> Suisho5 由来の HalfKP256 eval や KP256 eval は本パッケージとは互換性なし。

### LayerStack の bucket 規則

進行度のみで 0〜7 の 8 個を選ぶ (`progress8x`、相入玉用の bucket は無い)。
係数は `progress.bin` から読む。`LS_BUCKET_MODE` option は存在しない。

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
