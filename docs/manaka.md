# Manaka エディション

`YANEURAOU_ENGINE_MANAKA` は、Manaka の学習済み `nn.bin` を評価するエディション。
トークン集合の power sum と pair の和を整数で差分更新する。NNUE とは別の評価関数である。

**このフォーク専用の形式で、上流の [yaneurao/YaneuraOu](https://github.com/yaneurao/YaneuraOu)
では読み込めない。** このフォークでも NNUE エディションでは拒否する。

## ビルド

`source/` で実行する。

```sh
make clean YANEURAOU_EDITION=YANEURAOU_ENGINE_MANAKA
make -j normal YANEURAOU_EDITION=YANEURAOU_ENGINE_MANAKA TARGET_CPU=AVX2 COMPILER=g++
```

エディションを切り替えるときは、切り替え前後それぞれのエディションを指定して
`make clean` を実行する。コンパイル時の定義の違いは依存ファイルに含まれないため、
別エディションのオブジェクトを再利用しないこと。

AVX2 ビルドで確認している。AVX2 以外のターゲットと WASM での動作は未確認。

## 評価関数ファイル

対応する `ModelType` は次の 2 種。

| 形式 | 内容の検証 |
|---|---|
| `SYMq16c` | 末尾の CRC32C を必ず照合。不一致や trailer の欠落は拒否 |
| `SYMq16` (旧型) | チェックサムが無く、内容が未検証である警告を表示 |

対応する構造は、pair を持つ Tokens 空間の `pair16_d128_o3_h512` と
`pair16_d128_o3_h512_h2256` の 2 種だけ。それ以外は読み込みで明示的に拒否する。
駒落ちなど、トークンが 40 個でない局面には対応しない。
`SYM2` は不採用で、このエディションでも NNUE エディションでも読めない。

重みの書き出しは Manaka の Rust 側の `export-nnue` が行う。
通常の NNUE の `nn.bin` と名前が同じでも内容は異なる。重みは同梱しない。

## 速度

1 台の機械での実測。`bench 16 1 12 default depth`、`Threads` 1、`USI_Hash` 16、AVX2 ビルド、
1 コアに固定。他のプロセスが動いていたので、数字は数パーセント揺れる。

| 評価関数 | ノード数 | nps |
|---|---|---|
| `pair16_d128_o3_h512` | 954839 | 約 26.0 万 |
| `pair16_d128_o3_h512_h2256` | 940985 | 約 22.5 万 |

他の機械、他のスレッド数、AVX2 以外のターゲットでは測っていない。

## 読み込み

`EvalDir` は既定 `eval`、`EvalFile` は既定 `nn.bin`。
実行ファイルのディレクトリを基準に組み合わせる。絶対パスも指定できる。

```text
usi
setoption name EvalDir value eval/manaka
setoption name EvalFile value nn.bin
setoption name ManakaRequireChecksum value true
isready
```

`ManakaRequireChecksum` は既定 `false`。`true` ならチェックサムの無い旧 `SYMq16` を拒否する。
`SYMq16c` の CRC32C は、このオプションが `false` でも必ず照合する。
成功時は起動ログに `SYMq16c ... checksum_verified=verified` と表示する。
旧型では `checksum_verified=unverified` と内容未検証の警告を表示する。

読み込み失敗時は `Error!` を表示して終了コード 1 で終了し、`readyok` は返さない。
読めない重みをゼロ評価で代替しない。

開発用の f32 形式 `MNW1` は通常ビルドで拒否する。
開発用ビルドと検査コマンドは [接着 API](../source/eval/manaka/API.md) を参照。
