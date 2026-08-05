# YaneuraOu を Cloudflare Workers で動かす Wasm ビルド設計

## 目的

YaneuraOu を Cloudflare Workers 上で動作させる Wasm ビルドを用意する。元の Emscripten ビルドは評価関数 (NNUE) を `--preload-file` で wasm に埋め込んでおり、以下の課題があった。

- Workers のスクリプトサイズ上限 (paid: 10MB compressed) を超える
- 評価関数を差し替えるたびに再ビルドが必要
- 定跡ファイルが利用できない

本設計では評価関数・定跡を **外部ファイル化** し、Workers Static Assets / R2 から fetch して Emscripten の MEMFS に書き込む方式に変更する。

## 配布形態

`@ultemica/yaneuraou-cfworkers` として npm publish。Workers 側は

```ts
import { createEngine } from "@ultemica/yaneuraou-cfworkers";
```

の形で取り込む。バイナリ本体 (`yaneuraou.wasm`) は同パッケージに同梱、評価関数・定跡は **消費側のアプリ** が用意する (R2 / Static Assets)。

## ファイル構成

```
yaneuraou-cfworkers/
├ script/
│  └ build_cfworkers.sh        # Emscripten ビルドスクリプト
├ src/
│  └ loader.js                 # JS グルー (消費側エントリ)
├ dist/                        # ビルド成果物 (publish 対象)
│  ├ yaneuraou.mjs             # Emscripten 生成 ES module
│  ├ yaneuraou.wasm            # Wasm バイナリ
│  ├ yaneuraou.d.ts            # 型定義
│  └ loader.js                 # 上記の dist 版
├ package.json
└ README.md
```

## ビルドスクリプト

`script/build_cfworkers.sh`:

```bash
#!/usr/bin/env bash
set -eu

cd source

emcc \
  -O3 -msimd128 -DNDEBUG \
  -DUSE_EVAL_HASH -DENABLE_TEST_CMD -DUSE_AVX2=0 -DUSE_SSE42=0 \
  -DEVAL_NNUE -DEVAL_LEARN=0 \
  -DUSE_MAKEFILE \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_ES6=1 \
  -s EXPORT_NAME=createYaneuraOu \
  -s ENVIRONMENT=web,worker \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=64MB \
  -s MAXIMUM_MEMORY=128MB \
  -s STACK_SIZE=2MB \
  -s EXPORTED_RUNTIME_METHODS='["FS","callMain","ccall","cwrap","stringToUTF8","UTF8ToString","lengthBytesUTF8"]' \
  -s INVOKE_RUN=0 \
  -s EXIT_RUNTIME=0 \
  -s FORCE_FILESYSTEM=1 \
  *.cpp eval/**/*.cpp engine/yaneuraou-engine/*.cpp \
  -o ../dist/yaneuraou.mjs

echo "built: dist/yaneuraou.mjs + dist/yaneuraou.wasm"
```

### 旧ビルドからの主な差分

| 項目 | 旧 | 新 |
|---|---|---|
| 評価関数 | `--preload-file eval` で埋込 | 外部ファイル、JS 側で `FS.writeFile` |
| 出力形式 | UMD (`yaneuraou.js`) | ES module (`yaneuraou.mjs`) |
| pthread | `-pthread -s USE_PTHREADS=1` | 無効 (Workers は SAB 不可) |
| `INVOKE_RUN` | 1 (即 main 実行) | 0 (JS から `callMain()`) |
| `FS` の export | 無し | `EXPORTED_RUNTIME_METHODS` に追加 |

### フラグの意図

- `MODULARIZE=1` + `EXPORT_ES6=1`: `import createYaneuraOu from "./yaneuraou.mjs"` の形式に
- `ENVIRONMENT=web,worker`: Cloudflare Workers は Service Worker 互換ランタイム
- `INVOKE_RUN=0`: ファイル書き込みを終えてから手動で `callMain()` を呼ぶため
- `EXIT_RUNTIME=0`: UCI ループが return したあとも runtime を破棄しない
- `FORCE_FILESYSTEM=1`: 自動 tree-shake で FS が消えるのを防止
- `-msimd128`: V8 / Workers は wasm SIMD サポート済み、評価関数の行列演算で効く
- `INITIAL_MEMORY=64MB`: 評価関数 ~20MB + ハッシュ等を見越した初期確保
- `MAXIMUM_MEMORY=128MB`: Workers の上限

## JS グルー

`src/loader.js`:

```js
import createYaneuraOu from "./yaneuraou.mjs";

/**
 * @param {object} opts
 * @param {ArrayBuffer} opts.evalBin       評価関数の中身
 * @param {ArrayBuffer} [opts.bookDb]      定跡 (任意)
 * @param {string}      [opts.bookFile]    定跡ファイル名
 * @param {(line:string)=>void} opts.onLine  エンジン stdout コールバック
 */
export async function createEngine(opts) {
  const stdinQueue = [];
  let stdinBuf = null;
  let stdinPos = 0;

  const Module = await createYaneuraOu({
    print: opts.onLine,
    printErr: (l) => console.error("[engine]", l),
    stdin: () => {
      if (!stdinBuf || stdinPos >= stdinBuf.length) {
        const next = stdinQueue.shift();
        if (!next) return null;
        stdinBuf = new TextEncoder().encode(next + "\n");
        stdinPos = 0;
      }
      return stdinBuf[stdinPos++];
    },
    locateFile: (p) => new URL(p, import.meta.url).href,
  });

  Module.FS.mkdirTree("/eval");
  Module.FS.writeFile("/eval/nn.bin", new Uint8Array(opts.evalBin));

  if (opts.bookDb && opts.bookFile) {
    Module.FS.mkdirTree("/book");
    Module.FS.writeFile(`/book/${opts.bookFile}`, new Uint8Array(opts.bookDb));
  }

  Module.callMain([]);

  const send = (cmd) => stdinQueue.push(cmd);

  send("setoption name EvalDir value /eval");
  if (opts.bookFile) {
    send("setoption name BookDir value /book");
    send(`setoption name BookFile value ${opts.bookFile}`);
  }

  return {
    send,
    quit: () => send("quit"),
  };
}
```

### 動作の流れ

1. `createYaneuraOu()` で wasm をインスタンス化、`stdin` ハンドラを差し込む
2. 評価関数・定跡を `Module.FS.writeFile` で MEMFS に書き込み
3. `callMain()` で UCI ループを起動 (stdin 待ち状態に入る)
4. `send("setoption name EvalDir value /eval")` 等で読み込みパスを通知
5. C++ 側は YaneuraOu 既存の `EvalDir` / `BookDir` / `BookFile` UCI option を経由してファイルを fopen → 透過的に MEMFS から読まれる

C++ 側の改造は **不要**。既存の UCI option をそのまま使う。

## package.json

```json
{
  "name": "@ultemica/yaneuraou-cfworkers",
  "version": "0.1.0",
  "description": "YaneuraOu shogi engine for Cloudflare Workers (Wasm, no embedded eval)",
  "license": "GPL-3.0",
  "type": "module",
  "main": "./dist/loader.js",
  "types": "./dist/yaneuraou.d.ts",
  "files": [
    "dist/yaneuraou.mjs",
    "dist/yaneuraou.wasm",
    "dist/yaneuraou.d.ts",
    "dist/loader.js"
  ],
  "exports": {
    ".": {
      "types": "./dist/yaneuraou.d.ts",
      "default": "./dist/loader.js"
    },
    "./wasm": "./dist/yaneuraou.wasm"
  }
}
```

## 消費側 (Cloudflare Workers) の使い方

```ts
import { createEngine } from "@ultemica/yaneuraou-cfworkers";

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const evalBin = await (
      await env.ASSETS.fetch(new URL("/eval/nn.bin", req.url))
    ).arrayBuffer();

    const lines: string[] = [];
    const engine = await createEngine({
      evalBin,
      onLine: (l) => lines.push(l),
    });

    engine.send("usi");
    engine.send("isready");
    engine.send("position startpos");
    engine.send("go byoyomi 1000");

    // bestmove を待つ実装は別途
    return new Response(lines.join("\n"));
  },
};
```

評価関数の置き場所は

- **Workers Static Assets**: 25MiB/file 上限、CDN キャッシュあり、固定ファイル向き
- **R2**: サイズ無制限、ユーザーごとに切り替えたい定跡などに向く

を用途で使い分ける。

## 制約と注意点

| 項目 | 内容 |
|---|---|
| メモリ | Workers 全体 128MB、評価関数を halfkp_256x2 級 (~20MB) 以下に抑える |
| CPU 時間 | paid plan で 30 秒/req、長考は Durable Object + WebSocket で分割 |
| 並列探索 | pthread 不可なので `Threads=1` 固定 |
| 起動コスト | 評価関数ロードに ~200ms、Durable Object に常駐させて使い回す |
| 評価関数の差替 | 再ビルド不要、Static Assets を更新するだけ |

## ロードマップ

1. ビルドスクリプトを fork に追加し、CI で `dist/` を生成する GitHub Actions ワークフローを置く
2. `npm publish` を release タグ駆動で行う (GitHub Actions + `NODE_AUTH_TOKEN`)
3. 棋樂アプリ側で `@ultemica/yaneuraou-cfworkers` を依存追加し、`bot/` の YaneuraOu 起動経路を Wasm 版に置換可能か検証
4. 評価関数を Workers Static Assets に配置するパス設計
5. Durable Object 上での常駐起動 + WebSocket 経由の UCI ブリッジ
