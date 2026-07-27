# WASM パッケージのリリース手順

`.github/workflows/build-wasm.yml` が `wasm-v*` タグを push されると
発火して、20 個 (現状) の npm-shaped パッケージを並列ビルドし、tar.gz
として GitHub Release に並べる。

このドキュメントは **そのリリースに新しいパッケージを 1 つ追加するとき** の
最小ステップをまとめる。今日のセッションで Node 用パッケージを 6 個足したとき、
matrix / for ループ / files 列 / release body の表 / 数値 literal の **5 箇所**
を別々に編集しなくちゃいけなかったが、それを **matrix への 1 ブロック追加 だけ**
に圧縮する設計に作り替えてある。

---

## 新しいパッケージを追加するとき

### ステップ 1: パッケージディレクトリを作る

既存の sibling パッケージをまるごとコピーして名前を変える。パッケージは
すべて `outputs/` 配下に置く。例えば `outputs/yaneuraou-wasm-node-kp256`
を雛形にして `outputs/yaneuraou-wasm-node-foo` を作るなら:

```sh
cp -R outputs/yaneuraou-wasm-node-kp256 outputs/yaneuraou-wasm-node-foo
# package.json の name / description を foo 用に書き換え
# README.md / SPEC.md も同様
# src/index.ts と src/worker_shim.ts は基本そのまま (パッケージ名コメントだけ修正)
```

`dist/` 配下は CI がビルド時に生成するので空のままで OK (`.gitignore` に
`dist/` が入っているので残骸が混入する心配もない)。

### ステップ 2: `build-wasm.yml` の matrix に 1 ブロック追加

`.github/workflows/build-wasm.yml` の `matrix.package:` 配列に、新しい
エントリを 1 ブロック追加する。フィールド一覧:

| フィールド | 意味 | 例 |
|---|---|---|
| `name` | matrix エントリ識別子 (artifact 名にも使われる) | `node-foo` |
| `dir` | パッケージディレクトリ名 (`outputs/` 配下、プレフィックスは付けない) | `yaneuraou-wasm-node-foo` |
| `edition` | `YANEURAOU_EDITION=` に渡す | `YANEURAOU_ENGINE_NNUE_KP256` |
| `export_name` | `EM_EXPORT_NAME=` | `YaneuraOu_K_P` |
| `environment` | `EM_ENVIRONMENT=` | `web` / `web,worker` / `node` |
| `pthread` | `EM_PTHREAD=` (0 or 1) | `1` |
| `initial_memory` | `EM_INITIAL_MEMORY_SIZE=` (bytes) | `134217728` |
| `maximum_memory` | `EM_MAXIMUM_MEMORY_SIZE=` (bytes) | `1073741824` |
| `category` | リリース本文の分類 (Single Source of Truth) | `cfworkers` / `pthread` / `node` / `cfworkers-hlsl` / `pthread-hlsl` |
| `engine_label` | リリース本文の Engine 列 (人間が読む名前) | `"NNUE KP256 (Suishopetite)"` |
| `eval_note` | リリース本文の Eval data 列 | `"external (~873 KB)"` / `"**embedded**"` / `"none"` |
| `extra_make_args` | 任意。make に追加で渡す引数 | `"MATERIAL_LEVEL=1 USE_HUMANLIKE_OPTIONS=ON"` |
| `exported_runtime_methods` | 任意。`EM_EXPORTED_RUNTIME_METHODS=` を上書き(node variant のみ) | `"['FS','ccall','callMain']"` |
| `emsdk_version` | 任意。emscripten docker image タグを上書き(node variant 用) | `"3.1.43"` |

`category` の値はリリース本文で表をどのセクションに振り分けるかを決める:

- `cfworkers` / `pthread` → "Default (humanlike OFF)" 表
- `cfworkers-hlsl` / `pthread-hlsl` → "HumanLike SkillLevel (hlsl) variants" 表
- `node` → "Node.js variants" 表

未知の `category` 値は `script/generate_release_body.py` が **明示的に
エラー** で落とすので、typo はビルドが先に教えてくれる。

### ステップ 3: タグを push

```sh
git add outputs/yaneuraou-wasm-node-foo .github/workflows/build-wasm.yml
git commit -m "feat(wasm): add yaneuraou-wasm-node-foo"
git push origin develop
git tag wasm-v8.50.1
git push origin wasm-v8.50.1
```

CI が自動で:

1. 21 個 (新 1 個含む) を並列ビルド
2. それぞれを `${dir}-v8.50.1.tar.gz` にパッケージング
3. `script/generate_release_body.py` が matrix を読んで本文 Markdown を生成
   - 全パッケージ表に新 row が自動追加される
   - "Twenty" → "Twenty-one" / "Six" → "Seven" 等の literal も自動更新
4. `files: yaneuraou-wasm-*-v8.50.1.tar.gz` の glob で全 tar.gz を Release アセットに添付

---

## 触らなくて良いもの (重要)

新パッケージを足すために 過去に編集が必要だった以下の場所は、**もう触らない**:

| 場所 | 理由 |
|---|---|
| `release-wasm` の `for d in \ ...` ループ | matrix から `Parse matrix as SoT` ステップが dirs を流し込む |
| `release-wasm` の `files:` ブロック | `yaneuraou-wasm-*-v${version}.tar.gz` glob 1 行で網羅 |
| `release-wasm` の `body:` (Markdown 表) | `generate_release_body.py` が matrix から表を組み立てる |
| body 内の "Twenty" / "Six" / "fourteen" などの literal 数値 | generator が自動英訳 (`script/generate_release_body.py` の `_WORDS` 表) |

これらを **手で触ろうとしている** = 設計の意図に反している、警告サイン。

---

## 既存パッケージを 「同じ 20 個のまま別バージョンでリリース」 するとき

matrix も script も触らない。タグを切って push するだけ:

```sh
git tag wasm-v8.51.0
git push origin wasm-v8.51.0
```

emscripten 5.0.5 の docker image が新しくなった、YaneuraOu source/ を直した、
等の事情で再ビルドしたい場合はこれだけで十分。

`workflow_dispatch` で全 emscripten バージョンを上書きしたいときは、
GitHub Actions UI から `Build wasm` を手動実行し、`emsdk_version` 入力に
別バージョンを入れる:

```
Run workflow → emsdk_version: 5.0.6 → Run
```

ただし node variant (`emsdk_version: "3.1.43"` を matrix で固定している 6 個)
は **per-matrix 設定が dispatch 入力より優先される** ので、3.1.43 のまま
ビルドされる。これは設計通り(他バージョンでは Node が動かない)。

---

## emscripten バージョンの優先順位

`docker pull` と `make` 呼び出し時の `EMSDK_VERSION` 解決:

```
matrix.package.emsdk_version  >  inputs.emsdk_version  >  '5.0.5'
```

`exported_runtime_methods` も同様:

```
matrix.package.exported_runtime_methods  >  "['FS','ccall']" (デフォルト)
```

それ以外のフィールド (edition / export_name / environment / pthread /
memory / extra_make_args) は **matrix.package のみ** から取り、デフォルト
は無い。

---

## 参考: SoT 設計の責務分担

- **`.github/workflows/build-wasm.yml`** の `matrix.package` ── 全パッケージの
  存在と属性を定義する SoT。ここを編集すれば、ビルド・パッキング・
  本文・アセット glob のすべてに自動反映される。
- **`script/generate_release_body.py`** ── matrix を読んで Markdown 本文を
  組み立てるレンダラ。表のレイアウト、静的な散文 (Quickstart コード片、
  eval/book URL 表、注意書き)、`category` → セクション分けロジック、
  数値英訳辞書 (`_WORDS`) を持つ。
- **`docs/wasm_client_usage.md`** ── 利用者向け実装ガイド。リリース本文の
  Quickstart より詳しいことが書いてある。

新しい WASM パッケージ種別 (例: 新カテゴリ `node-hlsl`) を追加するときは、
generator の `_TARGET_BY_CATEGORY` 辞書にカテゴリを足し、`build_body()` 内の
セクション分岐に表を追加する。これは matrix 拡張だけでは賄えない **設計
変更** なので、その場合だけ generator を編集する。
