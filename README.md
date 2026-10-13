[![Build wasm](https://github.com/shielune/YaneuraOu/actions/workflows/build-wasm.yml/badge.svg)](https://github.com/shielune/YaneuraOu/actions/workflows/build-wasm.yml)

# YaneuraOu WASM

[yaneurao/YaneuraOu](https://github.com/yaneurao/YaneuraOu) のフォーク。将棋エンジン
やねうら王を **WebAssembly にビルドして npm 形の配布物にする**ことを目的にしている。
ブラウザ・Node.js・Cloudflare Workers で、ネイティブに近い速度で動く。

エンジン本体は upstream V9.60 ベース。フォーク独自のエンジンオプションもいくつか
足している (`docs/fork_engine_options.md`)。

## パッケージ

エンジン 5 種 × ランタイム 3 種の組み合わせで 10 パッケージを配布している。
最新版は [Releases](https://github.com/shielune/YaneuraOu/releases) の `tar.gz` から取得する。

| Package | 評価関数 | 並列 | Initial / Max memory | 動作環境 |
|---|---|---|---|---|
| `@ultemica/yaneuraou-wasm-pthread-kp256` | KP256 (外部 ~873 KB) | pthread | 128 MB / 1 GB | ブラウザ |
| `@ultemica/yaneuraou-wasm-pthread-halfkp256` | HalfKP_256x2_32_32 (外部 ~62 MB) | pthread | 256 MB / 2 GB | ブラウザ |
| `@ultemica/yaneuraou-wasm-pthread-halfkp768` | HalfKP_768x2_16_64 (外部 ~184 MB) | pthread | 256 MB / 2 GB | ブラウザ |
| `@ultemica/yaneuraou-wasm-mate-pthread` | 不要 (詰将棋 DfPn) | pthread | 128 MB / 1 GB | ブラウザ |
| `@ultemica/yaneuraou-wasm-node-kp256` | KP256 (外部 ~873 KB) | pthread | 128 MB / 1 GB | Node.js 18+ |
| `@ultemica/yaneuraou-wasm-node-halfkp256` | HalfKP_256x2_32_32 (外部 ~62 MB) | pthread | 256 MB / 2 GB | Node.js 18+ |
| `@ultemica/yaneuraou-wasm-node-halfkp768` | HalfKP_768x2_16_64 (外部 ~184 MB) | pthread | 256 MB / 2 GB | Node.js 18+ |
| `@ultemica/yaneuraou-wasm-node-mate` | 不要 (詰将棋 DfPn) | pthread | 128 MB / 1 GB | Node.js 18+ |
| `@ultemica/yaneuraou-wasm-kp256-cfworkers` | KP256 (外部 ~873 KB) | single | 64 MB / 128 MB | Cloudflare Workers |
| `@ultemica/yaneuraou-wasm-mate-cfworkers` | 不要 (詰将棋 DfPn) | single | 64 MB / 128 MB | Cloudflare Workers |

ランタイムごとにバイナリが別物なので、環境に合った variant を選ぶ必要がある。

- **ブラウザ (`pthread-*`)** — `SharedArrayBuffer` を使うので、配信側に
  `Cross-Origin-Opener-Policy: same-origin` と `Cross-Origin-Embedder-Policy: require-corp` が必要
- **Node.js (`node-*`)** — `EM_ENVIRONMENT=node` ビルド。ブラウザでは読み込めない
- **Cloudflare Workers (`*-cfworkers`)** — V8 Isolate は pthread を持てないので単一スレッド。
  128 MB heap に収まらない HalfKP は cfworkers variant を用意していない

評価関数は WASM バイナリに内蔵していない。`FS.writeFile` で MEMFS に書いてから
`EvalDir` で読ませる。入手先とサイズは
[`docs/wasm_client_usage.md`](docs/wasm_client_usage.md) にまとめてある。

## ネイティブ版 explain

`build-native-explain.yml` は explain 対応のタグから、Linux amd64 / arm64 と
macOS Intel / Apple Silicon の実行ファイルを作り、同じタグの Release に追加する。
各環境で KP256、HalfKP128、HalfKP256、HalfKP768、HalfKA1024、HalfKA2304 を用意する。
詰み専用エンジンは対象外。

| 環境 | ファイル名の環境部分 | Makefile の CPU 対象 |
|---|---|---|
| Linux x86-64 (SSE4.2 対応) | `linux-amd64` | `SSE42` |
| Linux AArch64 | `linux-arm64` | `ARMV8` |
| macOS Intel (11 以降、SSE4.2 対応) | `macos-amd64` | `APPLESSE42` |
| macOS Apple Silicon (11 以降) | `macos-arm64` | `APPLEM1` |

取得するアーカイブは
`yaneuraou-<環境>-<network>-<tag>-explain.tar.gz`。
例えば `yaneuraou-linux-amd64-kp256-v9.61.6-explain.tar.gz` に入る実行ファイルは
`yaneuraou-linux-amd64-kp256-explain`。
アーカイブには GPL ライセンスとソースのタグへのリンクを含める。
Linux 版は動的リンクなので、ビルド用 runner と互換性のある glibc / libstdc++ が必要。

`hce-progress.bin` と `hce-explain.bin` は、取得したアーカイブと同じタグの Release に
別の資産として載っている。その 2 つを取得して、展開した実行ファイルと同じディレクトリに置く。
2 つは同じ Release のものを組で使う（進行度ファイルとの指紋が合わないと内訳は出ない）。
モデルの再配布条件を変更せず、アーカイブには同梱しない。
そのディレクトリで実行ファイルを起動し、標準入力に送る。

```text
usi
setoption name HceProgressFile value hce-progress.bin
setoption name HceExplainFile value hce-explain.bin
position startpos
explain moves 7g7f 3c3d 8h2b+ 3a2b
quit
```

`explain` は根の局面と各着手後の局面を JSON 行として返す。
モデルの組が読み込めていれば `parts` に `material`、`tempo`、`threat`、`king_safety`、
`mobility` が入る。上の操作には NNUE の `nn.bin`、`isready`、`go` は不要。
探索を行うときは選んだ network に合う NNUE が別途必要で、`isready` が評価関数を読む。
出力形式（ヘッダ行、局面行、不正手のエラー行、終端の `done` 行、`legacy` / `top`）は
[docs/explain_protocol.md](docs/explain_protocol.md) に書いた。

### ネイティブ版の CI 前提

既存のリリースワークフローと同じ GitHub-hosted runner を使う。
Linux amd64 は `ubuntu-24.04`、Linux arm64 は `ubuntu-24.04-arm`、
macOS Intel は `macos-15-intel`、Apple Silicon は `macos-15`。
これらは [GitHub の標準 runner ラベル](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
で、独自の runner 登録は不要。ビルドには各イメージの make、g++ / Xcode の clang++、
python3 を使い、検証用の Node.js は 22.18.0 に固定する。

タグのソースに `HCE_EXPLAIN` と HCE の実装が必要で、無い場合は通常版を誤って公開せず失敗する。
手動実行は選んだブランチのソースを検証して Actions artifact に保存するだけで、
Release を作成・変更しない。手動実行のアーカイブ名ではタグの代わりに `dev-<commit>` を使う。
タグ push では全 20 組のビルドとネイティブ動作確認が成功してからアーカイブを追加し、
既存の WASM / Windows アセットとリリース本文は変更しない。
ビルドは `HCE_EXPLAIN=ON` と組み合わせ別の `OBJDIR` を使い、通常版のオブジェクトを再利用しない。
検証は SHA-256 を固定した既存のモデルを読み、NNUE 無しで通常の着手・捕獲・成りと
各局面の `parts` の五項目を確認する。

## 性能 — ネイティブと比べてどうか

「WASM だから遅い」ということはない。同一マシン・同一評価関数・同一探索条件で測ると、
**WASM はネイティブ SIMD ビルドの約 92% の探索速度**が出る。

| ビルド | nps | 対 native NEON |
|---|---|---|
| native clang-14 (NEON) | 764k | 102% |
| native gcc (NEON) | 748k | 100% |
| **wasm32 pthread** | **691k** | **92%** |
| wasm32 (SIMD なし) | 585k | 78% |
| native clang-14 (scalar) | 320k | 43% |
| native gcc (scalar) | 203k | 27% |

`Threads=1` / `go nodes 1000000` / KP256 + 水匠 petite / 3回の中央値。
絶対値はマシン依存なので、意味があるのは比のほう。

読み取れるのは次の3点。

- **WASM SIMD が効いている。** 切ると 691k → 585k (15% 減) まで落ちる。
  手書き SIMD の無い native scalar ビルド (203k〜320k) より WASM のほうが倍以上速い
- **スレッドは素直にスケールする。** Node で 1→4 スレッドが 686k → 2,916k (4.25×)。
  置換表の共有が効いて線形を超える
- **ブラウザと Node はほぼ同速。** 1スレッドで 99%、4スレッドで 92%。
  `SharedArrayBuffer` 同期のぶんブラウザがわずかに不利だが実用上は誤差

探索結果も **WASM 変種すべて (32/64bit・SIMD 有無・ブラウザ/Node/edge) でノード数までビット一致**する。
ランタイムを変えても読み筋は変わらない。

計測条件と生データは
[`docs/reports/2026-07-28_wasm_v96x_performance.md`](docs/reports/2026-07-28_wasm_v96x_performance.md)。

## 使い方 (Node.js)

```ts
import fs from "node:fs/promises";
import { createRequire } from "node:module";
import { createEngine } from "@ultemica/yaneuraou-wasm-node-kp256";
import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-kp256/engine";

const require = createRequire(import.meta.url);
const engine = await createEngine({
  factory: YaneuraOuFactory,
  enginePath: require.resolve("@ultemica/yaneuraou-wasm-node-kp256/engine"),
  wasmBinary: await fs.readFile(require.resolve("@ultemica/yaneuraou-wasm-node-kp256/wasm")),
  evalBin: await fs.readFile("./eval/nn.bin"),
  threads: 4,
  usiHash: 64,
});

const result = await engine.eval({
  sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
  byoyomi: 2_000,
});
console.log(result.bestmove, result.score);
engine.dispose();
```

ブラウザ・Cloudflare Workers 版の使い方、定跡の読み込み、USI オプションの効き方の
variant 差は [`docs/wasm_client_usage.md`](docs/wasm_client_usage.md) にある。

> ⚠ WASM ビルドは `USI_Hash` の既定値が **16 MB** (ネイティブは 1024 MB)。
> 1024 MB は WASM の `MAXIMUM_MEMORY` を超えて起動できないため下げている。
> 置換表を増やしたい場合、特に DfPn (`mate-*`) を使う場合は `setoption` で明示的に上げる。

## ツールチェイン

| variant | emscripten |
|---|---|
| `pthread-*` / `*-cfworkers` | 5.0.5 |
| `node-*` | 3.1.43 (固定) |

Node 版だけ古いバージョンに固定している。3.1.44〜3.1.73 は ESM worker で停止し、
3.1.74 以降は `INCOMING_MODULE_JS_API` で停止するため、pthread 付きのやねうら王を
Node で正しく動かせることを確認できている唯一のバージョンが 3.1.43 だから。
互換性マトリクスは [`docs/wasm_client_usage.md`](docs/wasm_client_usage.md) にある。

## ドキュメント

| 文書 | 内容 |
|---|---|
| [`docs/wasm_client_usage.md`](docs/wasm_client_usage.md) | WASM パッケージの利用ガイド (最初に読む) |
| [`docs/fork_engine_options.md`](docs/fork_engine_options.md) | 独自エンジンオプション (`FullTimeMode` / 隠しオプション / NNUE ヘッダ寛容化 / HumanLike) |
| [`docs/wasm_release_workflow.md`](docs/wasm_release_workflow.md) | WASM パッケージのリリース手順 |
| [`docs/releases/`](docs/releases/README.md) | 各リリースのリリースノート (本文の生成元) |
| [`docs/wasm_upgrade_changelog.md`](docs/wasm_upgrade_changelog.md) | emscripten / upstream 追従作業の記録 |
| [`docs/wasm_native_divergence.md`](docs/wasm_native_divergence.md) | native と WASM で探索結果が分かれる件の調査 |
| [`docs/nagisa_v3_diff_survey.md`](docs/nagisa_v3_diff_survey.md) | keinoda/YaneuraOu (NAGISA_V3) との差分調査 |
| [`docs/upstream_readme.md`](docs/upstream_readme.md) | upstream の README (大会戦績・解説記事一覧・関連リンク) |

## ライセンス

GPL-3.0。upstream やねうら王および Stockfish のライセンスを継承する。
