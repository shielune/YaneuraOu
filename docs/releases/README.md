# リリースノート

`wasm-v*` タグを push したときに GitHub Release の本文になる散文をここで管理する。

## 仕組み

リリース本文は 2 つの入力から組み立てられる。

| 内容 | どこから来るか |
|---|---|
| What's new の散文 (このリリース固有の話) | `docs/releases/<version>.md` |
| パッケージ表・メモリ・eval データ列 | `.github/workflows/build-wasm.yml` の matrix |
| Quickstart コード片、eval/book の URL 表、性能表 | `script/generate_release_body.py` の `STATIC_*` |

`script/generate_release_body.py --version 9.60.0` は `docs/releases/9.60.0.md` を
読んで冒頭に差し込み、残りを matrix と静的散文から生成する。ファイルが無ければ
バージョン非依存の汎用文にフォールバックする。

つまり **表の中身を直したいときは matrix を、このリリースで何をしたかを書きたい
ときはこのディレクトリのファイルを** 編集する。

## 書き方

`docs/releases/<version>.md` に Markdown をそのまま置く。見出しは付けない
(generator が `**What's new in this release**:` の位置に本文として差し込む)。
先頭にリード文 1 段落、続けて変更点の箇条書き、という形に揃えている。

CI を待たずに手元で確認できる:

```sh
python3 script/generate_release_body.py \
  --workflow .github/workflows/build-wasm.yml \
  --version 9.60.0 \
  --output /tmp/body.md
```

公開済みリリースの本文を後から差し替えるときも、このファイルを直してから

```sh
gh release edit wasm-v9.60.0 --repo shielune/YaneuraOu --notes-file /tmp/body.md
```

とすれば、リポジトリと公開物が食い違わない。

## explain の有るビルドと無いビルド

`explain` コマンドの有る版と無い版は、**同じ Release に並べて載せる**。タグは分けない。

- `build-wasm.yml` と `make-mingw.yml` の matrix に `explain: ['OFF', 'ON']` の軸がある。
  `OFF` が通常のビルド、`ON` が `HCE_EXPLAIN=ON` のビルド。
- 詰み専用の mate は評価関数がなく、`HCE_EXPLAIN=ON` を渡してもビルドに何も入らない
  (Makefile の NNUE の節の中だけで効く)。なので `exclude` で ON を作らない。
- tarball は、通常のものがこれまでの名前のまま、explain 入りのものは版のあとに `-explain` が付く。
  `yaneuraou-wasm-pthread-kp256-v9.61.1.tar.gz` と `yaneuraou-wasm-pthread-kp256-v9.61.1-explain.tar.gz`。
  Windows も同じ (`yaneuraou-windows-kp256-v9.61.1-explain.tar.gz`)。
- 中の `package.json` の名前と版は両方とも同じ。違うのはエンジン本体 (`yaneuraou.wasm` など) だけ。
- 本文の「explain の有るビルド」の節は `script/generate_release_body.py` が matrix から作る。
  どのパッケージに `-explain` があるかは matrix の `exclude` を読んで決まる。

## HCE のモデルファイル

`explain` が読む 2 つのファイルは `assets/hce-explain/` に置き、Release の資産として
tarball と並べて付ける (`build-wasm.yml` の `release-wasm` の `files:`)。

- `hce-progress.bin` (形式 `HCEPRG01`、`HceProgressFile` に渡す): 残り手数を当てる進行度ネット。
- `hce-explain.bin` (形式 `HCEXPL01`、`HceExplainFile` に渡す): 駒得・速度・玉の安全度・駒の働き・手番のモデル。
- NAGISA が `nn.bin` の隣に要求する `progress.bin` とは別物。取り違えないよう、名前に `hce-` を付けている。
- モデルは進行度ファイルとの対で学習してあり、両方の指紋 (進行度ファイル全体の FNV-1a 64) が合わないと
  `explain` は内訳を出さない。差し替えるときは 2 つ一緒に。
- 本文の表 (大きさと SHA-256) は `script/generate_release_body.py` がファイルそのものから作る。
  ファイルが無ければ「利用者が用意する」という文になる。
- バイナリ扱いにするため `.gitattributes` に `assets/hce-explain/*.bin binary` を足してある。
