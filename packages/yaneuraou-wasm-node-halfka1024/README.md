# @ultemica/yaneuraou-wasm-node-halfka1024

YaneuraOu SFNN HalfKA_hm2 1024x2-15-64 (NAGISA_V3 互換ネットワーク) 将棋エンジンを
**Node.js から** multi-thread (pthread) で動かすための WASM パッケージ。

- エンジン: YaneuraOu SFNN HalfKA_hm2 1024x2-15-64 / LayerStack 9 (V9.60)
- スレッド: 可変 (`Threads` USI option、最大 32)
- WASM SIMD: 有効
- メモリ: **256 MB initial / 4 GB max** / 2 MB stack
- 評価関数: ~75 MB の `nn.bin` と `progress.bin` を実行時に渡す (**2 ファイル必要**)
- ランタイム: **Node.js 18+ 専用** (`node:worker_threads`)
- emscripten: **3.1.43 固定** — Node 互換性が確認できている唯一のバージョン

## Usage (Node 18+)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-halfka1024";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-halfka1024/engine";

const require = createRequire(import.meta.url);
const wasmPath = require.resolve(
  "@ultemica/yaneuraou-wasm-node-halfka1024/wasm",
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
| NAGISA_V3 nn.bin + progress.bin | ~75 MB | https://github.com/keinoda/YaneuraOu/releases/tag/nagisa-v3.1 |

> **この評価関数は 2 ファイル必要。** 他のネットワークと違い、LayerStack の
> 選択に使う進行度係数が `nn.bin` ではなく別ファイルの `progress.bin` にある。
> `progressBin` を渡さないと bucket を計算できない。

> 単体配布は無く、リリースページの各プラットフォーム版アーカイブに
> `eval/nn.bin` と `eval/progress.bin` が同梱されている
> (3 種のどれでもよい。評価関数の中身は同一)。

> `FV_SCALE` はこのビルドでは既定で **28** (他のネットワークは 16)。
> 配布物の `eval_options.txt` が指定している値と同じなので、手で設定する必要はない。

> SFNN HalfKA_hm2 は HalfKP 系 / KP256 とは互換性なし。

### LayerStack の bucket 規則

`LS_BUCKET_MODE` USI option で切り替える。

| 値 | 規則 |
|---|---|
| `progress8kpabs` (既定) | 進行度のみで 0〜7 を選ぶ。9 個目の LayerStack は使わない |
| `progress8ek` | 相入玉局面を 9 個目 (index 8) に割り当てる |

既定は本家 NAGISA_V3 と同じ `progress8kpabs`。両者の結果が変わるのは
相入玉局面のみ。

## API

`yaneuraou-wasm-node-kp256` と同じ surface に `progressBin` が加わる。

## Files in this package

`yaneuraou-wasm-node-halfkp256` と同じ構成。

## License

GPL-3.0 (inherited from YaneuraOu)
