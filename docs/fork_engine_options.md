# このフォーク独自のエンジンオプション / 挙動

upstream やねうら王 V8.50 に対して、このフォークで追加したエンジンオプションと
挙動の変更点をまとめる。WASM パッケージ側の使い方は `docs/wasm_client_usage.md`、
リリース手順は `docs/wasm_release_workflow.md` を参照。

---

## FullTimeMode

| | |
|---|---|
| 型 | `check` |
| 既定値 | `false` |
| 対象 | やねうら王 通常探索部を持つ全 edition |
| 追加 | 2026-07-27 (`58ffa983`) |

反復深化のループは、基準思考時間 `Time.optimum()` に動的係数を掛けて
実際の打ち切り時刻を決めている。係数は

- `fallingEval` — 評価値が下降しているか (0.580〜1.667)
- `reduction` — 前回の思考時間短縮の度合い
- `bestMoveInstability` — root の最善手がどれだけ揺れたか

の 3 つで、**掛け合わせると 1 を下回ることが多い**。最善手が 8 手以上
安定していて評価値も落ちていない局面では 0.5 倍程度になり、`optimum` の
半分ほどで指してしまう。

`FullTimeMode` を `true` にすると、動的係数を**延長にだけ使い、短縮には
使わない**。すなわち

```
totalTime = max(optimum, optimum * fallingEval * reduction * bestMoveInstability)
```

となり、`optimum` より早くは止まらなくなる。局面が難しそうな場合
(係数の積が 1 を超える場合) は従来どおり延長する。

### いつ使うか

1 ノードあたりの計算コストが大きい評価関数で、持ち時間を余らせるのが
そのまま損になる場合。逆に軽量な評価関数では、余った時間を次の手に
回したほうが良いことも多いので既定は `false` のままにしてある。

合法手が 1 手しかないときの即指し (`totalTime = 0`) は `FullTimeMode` でも
そのまま働く。

### 効かないケース (重要)

**時間制御が働いている探索でのみ意味を持つ。** `Search::Limits::use_time_management()`
が `false` を返す探索、すなわち

- `go movetime N`
- `go depth N` / `go nodes N` / `go infinite` / `go mate N`

では `totalTime` の計算自体が行われないので、このオプションは何もしない。
`go btime ... wtime ...` (秒読み・フィッシャー含む) のときだけ効く。

WASM パッケージの `eval({ byoyomi })` は内部で `go movetime` を発行するので、
**現状の WASM 経路ではこのオプションは無効**。`usi` の応答には出てくるが
設定しても挙動は変わらない。

### 検証結果

MaterialLv1 / 1 スレッド / 安定局面で、`optimum = 2000ms` のとき:

| FullTimeMode | 動的係数適用後 | 実際の `totalTime` |
|---|---|---|
| `false` (既定) | 1063.76 ms | 1063.76 ms |
| `true` | 1063.76 ms | 2000 ms |

---

## 隠しオプション (開発者向け API)

配布ビルドの GUI 設定画面に出したくない実験用パラメーターのための仕組み。
**オプションそのものではなく、オプションを登録する側の API。**

| | |
|---|---|
| 追加 | 2026-07-27 (`75b16a1f`) |
| 実装 | `source/usi.h` (`Option::hidden()`)、`source/usi_option.cpp` |

```cpp
// source/engine/yaneuraou-engine/yaneuraou-search.cpp の USI::extra_option() 等で
o["ExperimentalFoo"] << Option(false).hidden();
```

- `setoption name ExperimentalFoo value true` は**通る**
- `usi` に対する `option name ...` 行には**出ない**

`Option::overwrite()` は `visible_in_usi` を `idx` / `on_change` と同様に
保存するので、`engine_options.txt` や `ENGINE_OPTIONS` で値を上書きしても
隠しオプションが表に出ることはない。

`USI::UnitTest` の `hidden options` セクションで、登録される・option 行に
出ない・`setoption` で設定できる、の 3 点を検証している
(`unittest` コマンドで実行)。

---

## NNUE 評価関数ファイルのヘッダ version 寛容化

| | |
|---|---|
| 対象 | NNUE 系 edition 全部 |
| 変更 | 2026-07-27 (`344530a2`) |
| 実装 | `source/eval/nnue/evaluate_nnue.cpp` の `ReadHeader()` |

外部の学習ツールが書き出した `nn.bin` は、パラメーターのレイアウトが
同一でもヘッダの version 定数だけが違うことがある。従来はここで
`FileMismatch` として弾いていたが、警告を出して読み進めるようにした。

```
info string NNUE header version mismatch: expected 2062757654 got 3735928559 (continuing anyway)
```

**安全性は落ちていない。** レイアウトの一致は

1. ヘッダ直後の `hash_value != kHashValue` の比較
2. FeatureTransformer / 各層の `GetHashValue()` の比較

で引き続き検証されるので、本当に非互換なファイルは従来どおり
`FileMissMatch` で拒否される。version だけが違う互換ファイルが
読めるようになっただけ。

---

## HumanLike personality オプション

`USE_HUMANLIKE_OPTIONS=ON` でビルドした edition (`-hlsl` パッケージ) でのみ
有効になる、棋風調整用のオプション群
(`ForceCaptureProb` / `StableKingProb` / `*BlindProb` 等)。

個々のオプションの意味は各 `-hlsl` パッケージの `README.md` / `SPEC.md` を参照。

---

## 出自

`FullTimeMode` / 隠しオプション / NNUE ヘッダ寛容化の 3 点は
[keinoda/YaneuraOu](https://github.com/keinoda/YaneuraOu) (NAGISA_V3) からの
移植。あちらは upstream V9.60 ベースなので、いずれも V8.50 の構造に
合わせて書き直してある。移植の可否を含む調査結果は
`docs/nagisa_v3_diff_survey.md` にまとめてある。
