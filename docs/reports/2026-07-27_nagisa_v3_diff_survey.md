# keinoda/YaneuraOu (NAGISA_V3 v3.1) 差分調査

調査日: 2026-07-27 / 対象タグ: `nagisa-v3.1` (`640f4656`)

## 結論の要約

| 関心領域 | 移植可否 | 理由 |
|---|---|---|
| 進行度SFNN評価関数 | **V9.60 追従が前提** | 依存する NNUE 基盤（SFNN アーキテクチャ、LayerStack、half_ka_hm2）がすべて upstream V9.60 で追加された。V8.50 には存在しない |
| SPSA 探索パラメータ調整基盤 | **ツールは可 / 調整値は不可** | Python ツール群は概ね独立。ただし `tune.h` が V9.60 由来で、調整値 146 個は V9.60 の探索コードに対応しており V8.50 に意味を持たない |
| USI オプション系 | **✅ 取り込み済み (2026-07-27)** | 3件とも小さく、V8.50 に対応箇所が存在した。詳細は下記 3 章と `docs/fork_engine_options.md` |

## 前提: 分岐構造

両者とも `yaneurao/YaneuraOu` のフォークで、共通祖先は **`73bc68ec`（2025-01-04, V8.50 直後）**。

```
73bc68ec (V8.50) ──┬── 我々 develop      : 独自24コミット (KIKI/Mobility, humanlike, WASM 20パッケージ)
                   └── nagisa-v3.1       : 383コミット
                                            ├─ upstream V8.50→V9.60 : 325 (yaneurao)
                                            └─ keinoda 独自          :  45
```

upstream V8.50→V9.60 の改造規模（我々が追いついていない分）:

| ファイル | 変更行数 |
|---|---|
| `yaneuraou-search.cpp` | 約 10,099 |
| `usi.cpp` | 約 2,560（`usioption.{h,cpp}` / `tune.{h,cpp}` に分割） |
| `book.cpp` | 約 1,544 |
| `evaluate_nnue.cpp` | 約 745 |
| `movepick.cpp` | 約 684 |

新規ファイルも `history.h`, `movegen.h`, `benchmark.cpp`, `half_ka*.{h,cpp}`, `sqr_clipped_relu.h`, `affine_transform_*_explicit.h`, dlshogi の `FukauraOuEngine.*` など多数。

---

## 1. 進行度SFNN評価関数（NAGISA の核心）

### 構成

- **NNUE アーキテクチャ**: HalfKA_hm2 1024x16x64 / LayerStack 9（`YANEURAOU_ENGINE_SFNN_halfkahm2_1024_15_64_ls9`）
- **進行度モデル**: `source/tanuki_progress.{cpp,h}`（keinoda 作、289行）+ `source/progress.bin`（約 1 MB のバイナリ）
- **バケット選択**: 進行度から LayerStack のインデックスを決める。`LS_BUCKET_MODE`（auto / kingrank9 / progress8kpabs / progress8ek）で実行時切替
- **時間管理連動**: 進行度に応じた SlowMover / Mtg（`timeman.cpp` 122行）

### 依存関係（ここが壁）

keinoda 自身のコードは `tanuki_progress.*` と統合部分だけで、その土台はすべて upstream V9.60 のもの:

| 必要なもの | 出自 |
|---|---|
| `sfnn-1536.h`（SFNN アーキテクチャ） | upstream `61d757e1` (2026-01-31, Hiroki Taniai #311) |
| `half_ka_hm2.{h,cpp}` | upstream `204f25fe` (2026-02-05) |
| LayerStack (`kLayerStacks`) | upstream。**我々の `source/eval/nnue/` には `LayerStack` の語が1つも無い** |
| `sqr_clipped_relu.h` / `affine_transform_*_explicit.h` | upstream V9.60 |
| `usioption.h`（`tanuki_progress.h` が直接 include） | upstream `96be9e0d` (2025-07-25) |

我々の `architectures/` にあるのは halfkp / halfkpe9 / kp / k-p 系のみで、SFNN 系は 1 つも無い。

### コスト評価

**V9.60 追従なしでは実質不可能。** 進行度モデルだけ移植しても、それを差し込む LayerStack が存在しない。

---

## 2. SPSA 探索パラメータ調整基盤

### 構成

| 要素 | 内容 | 移植性 |
|---|---|---|
| `script/spsa/{gen_params,apply_params,spsa_common}.py` | TUNABLE_PARAM 定義を読んで rshogi SPSA 用 params を生成 / 最終値をソースへ書き戻す | **高**（ソース非依存の Python） |
| `script/spsa/test_spsa_tools.py` | 上記のテスト（395行） | 高 |
| `docs/spsa-tuning.md` (keinoda 側) | 運用手順 | 高 |
| `source/tune.h` | `TUNABLE_PARAM` マクロ本体。USI オプション化の仕組み | **upstream V9.60 由来**（`96be9e0d`）。移植には V8.50 の `USI::Option` 実装に合わせた書き換えが必要 |
| `source/config.h` | `FOR_TOURNAMENT` と `ENABLE_TUNE` の排他チェック 6行 | 高 |
| 調整値 146 個（search.cpp 137 + movepick.cpp 9） | SPSA 実測値 | **不可** |

### 調整値が移植できない理由

パラメータは V9.40/V9.60 の探索コード構造に紐づいている。例えば `movepick.cpp` の同じ箇所:

```cpp
// nagisa (V9.60)                      // 我々 (V8.50)
m.value  = 2 * (*mainHistory)[us][m.raw()];          m.value  =     (*mainHistory)(pos.side_to_move(), m.from_to());
m.value += 2 * sharedHistory->pawn_entry(pos)[pc][to];  m.value += 2 * (*pawnHistory)(pawn_structure(pos), pc, to);
```

係数の初期値も、参照する history テーブルの API も違う。V9.60 で最適化された値を V8.50 のコードに入れても意味を持たない（悪化しうる）。

### 現実的な使い方

**ツールだけ持ってきて、我々のパラメータを自分で調整する**のが筋。`spsa_common.py` は
「固定2ファイルから TUNABLE_PARAM を N 個読む」という契約（`EXPECTED_PARAMETER_COUNT = 146`）
なので、対象ファイルと個数を我々の実態に合わせて書き換える。KIKI/Mobility や humanlike の
係数調整に転用できる。

必要な作業:
1. `tune.h` を V8.50 の `USI::Option`（`source/usi.h` にある）向けに書き直す — 中規模
2. `script/spsa/*.py` をコピーし、ファイルリストと期待個数を書き換え — 小
3. 調整したい箇所を `TUNABLE_PARAM(...)` に置き換え — 任意

---

## 3. USI オプション系（3件・すべて取り込み済み）

**2026-07-27 に 3 件とも移植完了。** 使い方は `docs/fork_engine_options.md` を参照。

| 項目 | nagisa 側 | 本リポの実装 | 備考 |
|---|---|---|---|
| FullTimeMode | `ee18f540` | `58ffa983` | V8.50 には `highBestMoveEffort` が無いので係数 3 つで構成 |
| 隠しオプション | `7a96b83b` | `75b16a1f` | `OptionsMap::add_hidden()` ではなく `Option::hidden()` として実装 |
| NNUE ヘッダ寛容化 | `23faf0a8` | `344530a2` | bucket 数スキップは LayerStack 前提なので不採用 |

以下は移植時の判断の記録。

### 3-1. FullTimeMode（`ee18f540` + `771fe811`）

思考時間の動的係数（評価値下降・最善手変動・ノード集中）を**延長にだけ使い短縮に使わない**モード。
`totalTime` が `optimum()` を下回らなくなる。大型評価関数で持ち時間を使い切りたいとき用。

- nagisa 側の変更: `yaneuraou-search.cpp` 30行
- **V8.50 に同じ構造が存在する**（`source/engine/yaneuraou-engine/yaneuraou-search.cpp:1526-1585` に `fallingEval` / `bestMoveInstability` / `totalTime`）
- 差分: V8.50 には `highBestMoveEffort` が無く、`mainThread->tm.optimum()` ではなく `Time.optimum()`
- **移植コスト: 小**（10行程度の手作業。`std::max(optimum, adaptiveTime)` を入れるだけ）
- 副次的な改善: 停止判定と `increaseDepth` 判定で同じ `stopTime` を使うよう整理している
  → **本リポでは採用しなかった**。V8.50 は `maximum()` による上限を `check_time()` 側で
  持っており、`stopTime` の導入は時間制御の別の箇所に踏み込むため、FullTimeMode の
  移植とは分けるべきと判断した
- 実測: `optimum=2000ms` の局面で動的係数適用後 1063.76ms → 有効時 2000ms
- 注意: `go movetime` / `go depth` 等では `use_time_management()` が false になり
  この計算自体が走らない。**WASM パッケージは `go movetime` を発行するので無効**

### 3-2. 非表示 USI オプション（`7a96b83b`）

`setoption` では設定できるが `usi` コマンドの option 行には出さないオプション。
実験用パラメータを配布ビルドから隠すのに使う（NAGISA_V3 はこれで実験オプションを非表示化）。

- nagisa 側の変更: `usioption.{h,cpp}` に `visible_in_usi` フラグと `add_hidden()` を追加、41行
- 我々は `usioption.*` が無く `Option` / `OptionsMap` は `source/usi.h:51` にある
- **移植コスト: 小〜中**（`Option` にフラグ追加、`operator<<` でスキップ、`add_hidden` 相当を用意。V8.50 の OptionsMap は API が違うので手作業）
- 実装: V8.50 の `OptionsMap` は素の `std::map` で登録が `o["名前"] << Option(...)` 形式なので、
  `add_hidden()` ではなくチェーンできる `Option::hidden()` として実装した
- **原典に無い追加修正**: `Option::overwrite()` は `*this = o` するため、`engine_options.txt`
  からの上書きで隠しオプションが表に出てしまう。`idx` / `on_change` と同様に
  `visible_in_usi` も保存するようにした

### 3-3. Tatara 形式 NNUE ヘッダの寛容化（`23faf0a8` + `4a0aa76f` の一部）

学習系（Tatara 等）が書き出した `nn.bin` は version 定数が違ったり、architecture 文字列の直後に
bucket 数（uint32）が入ったりする。これを弾かず警告のみで読み進める。

- 我々の該当箇所: `source/eval/nnue/evaluate_nnue.cpp:114`
  ```cpp
  if (!stream || version != kVersion) return Tools::ResultCode::FileMismatch;
  ```
  → 我々は version 不一致で即エラー。nagisa は警告のみで続行
- bucket 数スキップの判定は `FeatureTransformer::GetHashValue()` と突き合わせる方式
- **移植コスト: 小**。ただし **bucket 数の部分は LayerStack 前提なので不要**。
  version 寛容化（数行）だけ取れば、外部学習ツール製の `nn.bin` を読めるようになる
- 実装: 予定どおり version 寛容化のみ採用。レイアウト検証は直後の
  `hash_value != kHashValue` と各層の `GetHashValue()` が担うので安全性は不変
- 検証: version を `0xDEADBEEF` にした偽ヘッダで、警告を出して続行した後
  hash 検証で `FileMissMatch` になることを確認

---

## 参考: 今回調査していない keinoda 独自機能

| 機能 | 概要 | 見立て |
|---|---|---|
| 定跡グラフの千日手対策 | 定跡内の手待ちサイクル検出。`book.cpp` 141行 + `position.h` | 中（book.cpp は V9.60 で 1544行変更済みのため要手作業） |
| opening-target SFEN 探索バイアス | 特定局面へ誘導 | 未調査 |
| Windows AVX2/SSE4.2 配布 CI | GitHub Actions | 参考程度（我々は WASM 中心） |
| android EvalDir 解決 | eval ファイル探索パスの改善 | 小 |

## 再現用コマンド

```sh
git remote add nagisa https://github.com/keinoda/YaneuraOu
git fetch nagisa 'refs/tags/nagisa-v3.1:refs/tags/nagisa-v3.1'
git log --oneline 73bc68ec..nagisa-v3.1 | grep -v yaneurao   # keinoda 独自コミット
```
