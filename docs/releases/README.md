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

## 同じ版を二度出す (explain の有る版と無い版)

同じ版を、`explain` コマンドの無い `vX.Y.Z` と、有る `vX.Y.Z-explain` の二つのタグで出す。

- タグの末尾が `-explain` のときだけ、NNUE のパッケージを `HCE_EXPLAIN=ON` でビルドする。
  WASM も Windows も同じ。詰み専用の mate は、どちらのタグでも OFF のまま。
- パッケージの名前は変わらない。Release が二つできて、tarball の名前の版が
  `-v9.61.1.tar.gz` と `-v9.61.1-explain.tar.gz` で分かれる。
- `-explain` の Release は「最新」にしない。`latest` は explain の無い版を指す。
- 散文も版ごとに別のファイル: `docs/releases/X.Y.Z.md` と `docs/releases/X.Y.Z-explain.md`。
  `--version 9.61.1-explain` で後者を読む。
- タグを打たずに試すときは、`build-wasm.yml` と `make-mingw.yml` の
  `workflow_dispatch` の `explain` 入力を真にする。
