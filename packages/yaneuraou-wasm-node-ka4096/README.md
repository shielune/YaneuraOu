# @ultemica/yaneuraou-wasm-node-ka4096

YaneuraOu SFNN ka2 4096x2-7-64 (つようら王11 mini 互換ネットワーク) 将棋エンジンを
**Node.js から** multi-thread (pthread) で動かすための WASM パッケージ。

- エンジン: YaneuraOu SFNN ka2 4096x2-7-64 / LayerStack 8
- スレッド: 可変 (`Threads` USI option、ビルドの worker pool は 32)
- WASM SIMD: 有効
- メモリ: **256 MiB initial / 4 GiB max** / 16 MiB per-thread stack
- 評価関数: `nn.bin` (7,711,372 bytes) と `progress.bin` (1,003,104 bytes) を実行時に渡す (**2 ファイル必要**)
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-ka4096";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-ka4096/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve(
  "@ultemica/yaneuraou-wasm-node-ka4096/wasm",
);
const wasmBinary = await fs.readFile(wasmPath);
const evalBin = await fs.readFile("./eval/nn.bin");
const progressBin = await fs.readFile("./eval/progress.bin");

const engine = await createEngine({
  factory: YaneuraOuFactory,
  wasmBinary,
  evalBin,
  progressBin,
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
| つようら王11 mini nn.bin | 7,711,372 bytes | やねうら王 FANBOX (MIT ライセンスで公開) |
| progress.bin | 1,003,104 bytes | 同上 |
| 合計 | 8,714,476 bytes | |

> **この評価関数は 2 ファイル必要。** 他のネットワークと違い、LayerStack の
> 選択に使う進行度係数が `nn.bin` ではなく別ファイルの `progress.bin` にある。
> `progressBin` を渡さないと bucket を計算できない。

> [公式配布記事](https://yaneurao.fanbox.cc/posts/12761272) の mini 用 `.7z` から
> `nn.bin` と `progress.bin` を別途取得する。WASM パッケージには同梱しない。

> `FV_SCALE` は **24** を指定すること。このビルドの既定は 28 だが、つようら王11 は 24 前提。
> `createEngine` の `fvScale` オプション (既定 24) が `setoption name FV_SCALE` を送る。

> SFNN ka2 は HalfKP 系 / KP256 とは互換性なし。

### LayerStack の bucket 規則

進行度のみで 0〜7 の 8 個を選ぶ (`progress8x`、相入玉用の bucket は無い)。
係数は `progress.bin` から読む。`LS_BUCKET_MODE` option は存在しない。

## API

`yaneuraou-wasm-node-kp256` と同じ surface に `progressBin` と
`fvScale` (既定 24) が加わる。

## Files in this package

`yaneuraou-wasm-node-halfkp256` と同じ構成。

## License

GPL-3.0 (inherited from YaneuraOu)
