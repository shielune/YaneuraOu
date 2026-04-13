# WASM ビルド検証プロジェクト — 現状と今後の計画

## 背景と目的

`feature/wasm` ブランチでは YaneuraOu を emscripten 経由で WASM にビルドしている。過去に `emscripten/emsdk:3.1.43` + 旧 YaneuraOu ソースの組み合わせで動作していたが、emscripten を新しいバージョンに上げると「エンジンが局面を誤評価する」症状が出るとのメモがある。目的は次のとおり:

1. 既知良好版 (3.1.43) の WASM ビルドを再現し、評価値が正しいことをベースラインとして確定する。
2. emscripten を段階的にアップグレードしながら再ビルドし、**どのバージョンから評価値がずれるか** を二分する。
3. YaneuraOu 本体も段階的に新しくして同様の検証を行う。
4. 原因が特定できたら修正 (上流報告 or 回避パッチ) を行う。

## 実施済み作業

### 1. ビルド環境の確認

- Dev Container 内に em++ 5.0.5 がローカルインストール済み (`/usr/local/emsdk/`)。ただし `node` は PATH に無く、emsdk バンドルの `/usr/local/emsdk/node/22.16.0_64bit/bin/node` を使えばよい。
- Docker-outside-of-docker なので、ホスト側パスを指定しないと bind mount が効かない:
  - devcontainer 内 `/home/vscode/app` ↔ ホスト `/Users/devonly/Developer/personal/YaneuraOu`
  - `/proc/self/mountinfo` で確認可能。

### 2. マルチバージョン ビルドドライバ (`script/wasm_multibuild.sh`)

`emscripten/emsdk:<ver>` を順に使って `script/wasm_build.js k-p` を走らせるシェルスクリプトを作成。ログは `build/multibuild_logs/<ver>_<pkg>.log` に個別出力。

対象バージョン (2026-04-13 時点):

```
3.1.50, 3.1.60, 3.1.70, 3.1.74, 4.0.0, 4.0.11, 4.0.23, 5.0.0, 5.0.5
```

(3.1.43 は Makefile の `make build` で単体ビルド済み)

### 3. ビルド成果物の状態

| emscripten | アーキ   | 成果物                                   | 備考                                  |
|-----------:|:---------|:-----------------------------------------|:--------------------------------------|
| 3.1.43     | x86_64   | `yaneuraou.k-p.js` + `.wasm` + `.worker.js` | 別ファイル worker (旧スタイル)        |
| 3.1.50     | x86_64   | `.wasm` のみ (main `.js` 欠落)           | **要調査** — ビルド失敗               |
| 3.1.60     | x86_64   | `.js` + `.wasm`                          | worker はメイン JS に内蔵             |
| 3.1.70     | x86_64   | `.js` + `.wasm`                          | 同上                                  |
| 3.1.74     | x86_64   | `.js` + `.wasm`                          | 同上                                  |
| 4.0.0      | x86_64   | `.js` + `.wasm`                          | 同上                                  |
| 4.0.11     | x86_64   | `.js` + `.wasm`                          | 同上                                  |
| 4.0.23     | aarch64  | `.js` + `.wasm`                          | ここから emscripten docker image が arm64 native に |
| 5.0.0      | aarch64  | `.js` + `.wasm`                          | 同上                                  |
| 5.0.5      | aarch64  | `.js` + `.wasm`                          | 同上                                  |

`_x86_64` / `_aarch64` の接尾辞は `wasm_build.js` が `uname -m` をそのまま使っているだけで、出力される wasm 自体は理想的にはアーキ非依存のはず。ただし「4.0.23 を境に emcc/clang/llvm のビルド環境がエミュレーションから native へ変わる」ので、この境目で差が出た場合は emscripten バージョンだけでなくホストアーキの交絡要因も疑うこと (`--platform linux/amd64` で強制エミュレーションビルドすると切り分けられる)。

### 4. ベースライン eval (3.1.43, 30 秒)

テスト局面:
```
lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1
```

Node + Worker ポリフィル経由で実行 (Threads=1, USI_Hash=64):
```json
{
  "version": "3.1.43_x86_64",
  "score":   { "kind": "cp", "value": 392 },
  "bestmove": "bestmove G*9g ponder 9h9g",
  "depth": 23, "seldepth": 43, "nodes": 17148160
}
```

この `cp 392 / G*9g` を「正しい評価」とみなし、他バージョンとの比較対象にする。

### 5. Node での実行を試みたが 3.1.60+ で失敗

`script/wasm_eval_test.mjs` + `script/wasm_eval_worker_shim.mjs` で Node から WASM エンジンを直接叩く方式を用意。

- **3.1.43 は成功** — worker.js が独立した classic script で、indirect eval で worker_threads 上にロードできる。
- **3.1.60+ は失敗** — worker エントリがメイン .js (ES module) に統合され、pthread モードは `globalThis.self.name === 'em-pthread'` で判定される。shim で `name`, `importScripts`, `self` などを揃えて import しても、pthread worker が wasm を自前でフェッチしようとして `LinkError: memory import must be a WebAssembly.Memory object` で落ちる。

これは Emscripten の pthread プロトコルで、本来:
1. メインスレッドが Worker を作成、`{cmd: 'load', wasmMemory, wasmModule}` を postMessage
2. Worker はメッセージを受けて `Module.wasmMemory` / `Module.wasmModule` を共有メモリ付きで設定
3. ユーザー指定 `Module.instantiateWasm` が promise で返っていた分を、`va(wasmModule)` コールバックで解決

という流れだが、3.1.60 の minified コードでは `instantiateWasm` の「設定側」しか grep で見つからず、読み出し側が Closure 最適化で潰されている疑い。詳しく reverse engineering するには minified JS と格闘する必要があり、投資対効果が合わない。

## 既存スクリプト

| ファイル                              | 役割                                              | 状態           |
|:--------------------------------------|:--------------------------------------------------|:---------------|
| `script/wasm_build.js`                | 各バージョン向けビルド (既存、本プロジェクト変更無し)       | -              |
| `script/wasm_multibuild.sh`           | 複数 emscripten バージョンを順次ビルドするドライバ      | 動作 OK        |
| `script/wasm_eval_test.mjs`           | Node 経由で WASM エンジンに USI コマンドを流し eval を出す | 3.1.43 のみ OK |
| `script/wasm_eval_worker_shim.mjs`    | `worker_threads` で web worker を模倣する shim         | 3.1.43 のみ OK |
| `script/wasm_eval_all.sh`             | 全ビルドに対して順番に eval テストを流すバッチ         | 実行は未確認   |

## 今後の方針

### 方針決定: Playwright (Headless Chromium) で検証する

Node ポリフィルアプローチは 3.1.60+ で Emscripten pthread 実装との相性が悪く、深追いコストが高い。一方 `source/Makefile` 側を `ENVIRONMENT=web,worker` → `web,worker,node` にして再ビルドする案もあるが、ソース側に手を入れたくない (純粋に外部ツールチェーン差分を検証したい)。

**Playwright を導入してヘッドレス Chromium で WASM を動かす** のが最も素直。Chromium は pthread + SharedArrayBuffer + WebAssembly のフルスタックを網羅しているので、追加のシム無しで全バージョンの WASM をそのまま実行できる。

### 前提の環境整備 (ユーザー側で実施中)

- Dev Container に Bun (or Node) を入れる
- `bun add -D playwright`
- `bunx playwright install chromium`

### テストコード実装計画

1. **静的サーバ `script/wasm_eval_server.ts`** (仮称)
   - `build/<ver>/<pkg>/lib/` を root に `http://localhost:<port>` で配信
   - `cross-origin-isolation` を有効にするため、レスポンスに次のヘッダを付ける必要がある (pthread/SAB 要件):
     - `Cross-Origin-Opener-Policy: same-origin`
     - `Cross-Origin-Embedder-Policy: require-corp`
   - 任意のポートで listen し、テストランナーが port を取得して使う (起動/停止はテストから制御)
   - Bun の `Bun.serve` か素の Node `http` で十分。
2. **ドライバ HTML `script/wasm_eval_runner.html`** (仮称)
   - `<script type="module">` でクエリパラメータから `?engine=/yaneuraou.k-p.js` を受け取り、動的 import
   - 先述のベースライン SFEN をセットし、`go btime 0 wtime 0 byoyomi <ms>` を投げる
   - `info score ...` と `bestmove` を `window.__result` に書き込み or `postMessage` でテストに返す
3. **Playwright ランナー `script/wasm_eval_browser.ts`** (仮称)
   - 引数: `yaneuraou.<pkg>.js` の path と `--think-ms`
   - 該当 lib ディレクトリを静的サーバで公開 → Chromium を起動 → runner.html を開く → 結果を回収 → JSON で stdout に出す
   - 既存の `wasm_eval_test.mjs` と同じ出力フォーマットにすれば `wasm_eval_all.sh` をほぼそのまま流用できる
4. **バッチ `script/wasm_eval_all.sh` の差し替え**
   - `NODE_BIN` 分岐を消して `bun script/wasm_eval_browser.ts ...` に置き換え
   - 失敗 (3.1.50 のような) は null スコアで結果行を残して続行
5. **比較/サマリー出力**
   - 全バージョンの `score`, `bestmove`, `depth`, `nodes`, `nps` を 1 行/バージョンで表示
   - 3.1.43 を baseline として、scoreの diff (cp単位) と bestmove の一致を強調
   - 可能なら Markdown テーブルで `docs/wasm_eval_results.md` に追記 (人手判定で残しておきたい)

### 検証対象

- **初回**: 既存の 9 バージョン (3.1.50 は除外 or 再ビルド)
- **二分**: 最初に cp 392 から大きくずれるバージョンを見つけたら、その前後をさらに細かく刻む (e.g. 3.1.50, 3.1.55, 3.1.58...)
- **YaneuraOu バージョンアップ**: emscripten を固定 (良好版) した上で、YaneuraOu 本体を段階的に進める。`source/` を別 worktree でバージョン切り替えするのが楽。

### 3.1.50 の main `.js` 欠落問題

現状、3.1.50 のビルドは `.wasm` だけ残って `.js` が無い。`wasm_build.js` の圧縮ステップ (`fs.createReadStream().pipe(...)`) が非同期で、`process.exit(1)` に叩き落とされて中途半端に残った可能性。原因調査 & 単体再ビルドが必要だが優先度は低い (3.1.43 → 3.1.60 で既に跨いでおり、二分の粒度的に 3.1.50 は後回しでよい)。

### 調査に役立ちそうな Tips

- **Chromium の DevTools Protocol** で `chrome://flags` や `chrome://gpu` を開かなくても、起動時フラグで `--enable-features=SharedArrayBuffer` などを渡す必要があるかも。Playwright は Persistent Context なら `headless: false` で目視確認も可能。
- **3.1.43 のベースラインは 1 回 3884 ms で cp 417, 30 秒フルで cp 392 / depth 23** — 探索時間に応じて score が変動するので、**比較は同じ `think-ms` で揃える** こと。Hash サイズ・Threads も揃える。
- **決定性**: YaneuraOu の探索はデフォルトで完全決定的ではない (タイマー割り込み等)。**再現性が必要なら `go nodes N` に切り替える** ことを検討 (例: `go nodes 5000000`)。ただしエンジン側が `nodes` コマンドを理解するか要確認。
- **pthread 無しビルドを別途用意する手** — `source/Makefile` から `-pthread` を外したブランチを作れば Node で素直に動く。ただし本来の評価関数探索経路とは微妙に違うのでデバッグ用途に限定。

## 未解決 TODO

- [x] Bun / Node を Dev Container に導入
- [x] Playwright + Chromium のインストール
- [x] 静的サーバ + runner.html + browser ランナー の 3 点実装 (`script/wasm_eval_browser.ts`, `wasm_eval_runner.html`)
- [ ] 3.1.50 の単体再ビルド (優先度低)
- [x] `wasm_eval_all.sh` を Playwright ベースに差し替え
- [x] 9 バージョンで eval を回し、baseline と比較 → 詳細は `docs/wasm_eval_results.md`
- [ ] 評価値がずれる境界バージョンの特定、さらに細かい二分 → **現状は「3.1.70 と 3.1.43 は完全一致、3.1.74 以降は評価値以前に実行すら失敗」** なので、二分の前に 3.1.74 の起動不良を直す必要あり
- [ ] 必要なら YaneuraOu 本体のバージョンも段階的に進めて再検証
- [ ] 原因候補の切り分け (emscripten のバージョンか、ビルドホストアーキ x86_64 ↔ aarch64 か)
- [x] 結果を `docs/wasm_eval_results.md` に表で残す

## 検証結果サマリ (2026-04-13)

詳細は `docs/wasm_eval_results.md`。要点のみ:

- **3.1.43**: `cp 381 / G*9g / depth 24 / 22.86M nodes` (30秒) ← baseline
- **3.1.70**: `cp 381 / G*9g / depth 24 / 22.64M nodes` (30秒) ← baseline と一致
- **3.1.60**: ロード時 `Ga is not a function` — ES module worker での pthread 判定バグ (`B="function"==typeof importScripts`)
- **3.1.74, 4.0.0, 4.0.11, 4.0.23, 5.0.0**: WASM ロードは通るが `go` 後にワーカーからの出力が一切無し。emscripten が `INCOMING_MODULE_JS_API` のデフォルトから `print/printErr/postRun` を外したため `wasm_pre.js` の仕組みが機能しなくなった疑い
- **5.0.5**: 生成 JS 自体にセミコロン抜けがあり構文破綻 (`..."em-pthread"(function(){...`)

**つまり emscripten 3.1.70 までは評価値が一致しており、「評価値のずれ」ではなく「3.1.74 以降でビルドが機能しない」という別問題を先に解決する必要がある。**
