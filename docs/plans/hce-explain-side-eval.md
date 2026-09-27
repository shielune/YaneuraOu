# explain を NNUE 版に入れるビルドオプション (HCE_EXPLAIN)

## 目的

`explain` は、HCE v2 の線形評価（駒得 17、利き M、玉の安全度 50）を、指し手の列に沿って厳密に分解して JSON で出すコマンドである。今は KIKI edition（`EVAL_MOBILITY`）でしか使えない。

これを、普段使う NNUE 版にビルドオプション一つで足せるようにする。

```sh
make YANEURAOU_EDITION=YANEURAOU_ENGINE_NNUE HCE_EXPLAIN=ON
```

## 決めたこと

- Makefile の変数は、既存の `EVAL_EMBEDDING = OFF` と同じ ON/OFF の形にする。既定は OFF。ON にすると `-DUSE_HCE_EXPLAIN` が付く。
- HCE は explain のときだけ動く副評価器として組み込む。探索からは呼ばない。**ON と OFF で探索結果は同じ**でなければならない。
- rank+contact の重み（4474 個、float32 で 17,896 バイト）をバイナリに埋め込む。
  - fork の NNUE の埋め込みは INCBIN だが、ソースに MSVC では動かないと書いてある。そこで、C++ の配列を書いた .cpp を生成してコミットする。
- `HceWeightsFile` で、ほかの版の重みに差し替えられるようにする。空にすると埋め込みの重みに戻る。
- 組み込むのは NNUE 系の edition だけにする。
- **KIKI 版の explain は外す。** explain の本体は新しいファイル一か所に置き、KIKI 版からは消す。
  - KIKI 版の探索（`HceRoute`、`HceWeightsFile` など）には手を入れない。KIKI 版はビルドが通ることだけを確かめる。
- 配布物（WASM や CI）で ON にするかは、今回は決めない。必要な人が自分で指定する。
- `humanlike_eval.cpp` は分割しない。サイズが問題になったら考える。
- **`eval/mobility/` を `eval/hce/` に、`evaluate_mobility.cpp` を `evaluate_hce.cpp` に改名する。**
  - このファイルは駒得、利き、玉の安全度の三つで評価しており、利きだけを扱うものではない。
  - やねうら王では、edition の `Eval::` の入口を定義するファイルを `eval/<名前>/evaluate_<名前>.cpp` と呼ぶ（`eval/nnue/evaluate_nnue.cpp`、`eval/material/evaluate_material.cpp`、`eval/kppt/evaluate_kppt.cpp`）。この形に揃える。
  - explain の新しいファイルも同じ `eval/hce/` に置く。
  - 変えないもの:
    - マクロ `EVAL_MOBILITY` と edition 名 `YANEURAOU_ENGINE_KIKI`。config.h、usi.cpp、yaneuraou-search.cpp など 7 ファイルに広がるので、今回は触らない。
    - `mobility_weights_embedded.cpp`。中身は利きだけの 164 個の重みなので、名前のとおりである。
    - WASM のパッケージ名 `mobility` と export 名 `YaneuraOu_Mobility`。配布物の名前は変えない。

## 今のコード

行番号とパスは a71df8ac 時点のもの（改名前）。パスは `source/` からの相対パス。

### explain の本体

`eval/mobility/evaluate_mobility.cpp` は、ファイル全体が `#if defined(EVAL_MOBILITY)` で囲まれている。このファイルは `Eval::evaluate` や `Eval::add_options` など、NNUE 版と同じ名前の関数を定義している。そのため、NNUE 版にはリンクできない。

explain に要る部分は次のとおり。

| 部分 | 行 |
|---|---|
| `read_float_list`、`load_linear_weights` | 216〜237 |
| `effective_weights` の linear の分岐 | 298〜315 |
| `json_quote`、`num`、`see_value` | 467〜497 |
| `Snapshot`、`take_snapshot`、`top_movers`、`emit_node` | 500〜577 |
| `Eval::hce_explain` | 582〜648 |

`emit_node` と `hce_explain` は、KIKI の状態（`g_route`、`g_linear_*`、`g_file_bias`、`g_opt_bias`）を読んでいる。

### `eval/mobility` を指しているところ

fork の中:

| 場所 | 中身 |
|---|---|
| `Makefile` 423〜424 行目 | KIKI の `SOURCES` |
| `eval/humanlike/humanlike_eval.cpp` 467 行目 | コメント |
| `packages/yaneuraou-wasm-mobility-cfworkers{,-hlsl}/{README,SPEC}.md` 11〜13 行目 | 同梱の重みのパス（4 か所） |
| `packages/yaneuraou-wasm-node-mobility/SPEC.md` 23 行目 | `eval/kiki/...` と書いてあり、今もすでに間違っている |
| `script/wasm_build.js` 78 行目 | コメント。これも `eval/kiki/...` で、すでに間違っている |

`#include` は `../../config.h` や `../humanlike/humanlike_eval.h` のような相対パスで、ディレクトリの深さが変わらないので直さなくてよい。

### NNUE 版に持ち込めるもの

- `eval/humanlike/humanlike_eval.cpp` には edition のガードが無い。定義はすべて `Eval::HumanLike` の中にあるので、NNUE 版の名前とは衝突しない。
  - 末尾（1051〜1056 行目）で取り込んでいる `mobility_dump.cpp` も、`Position` と `HumanLike` しか使っていない。そのまま NNUE 版でコンパイルできる。
- `see` に要る `Position::see_ge` と `Eval::PieceValue` は、NNUE 版にもある（config.h 378〜393 行目）。
  - `PieceValue` を書き換えるのは `EVAL_MATERIAL` だけなので、`see` の値は KIKI 版と同じになる。

### ガードとオプションの登録

- explain のガードは usi.cpp 416 行目と 1421 行目、usi.h 201 行目、evaluate.h 47 行目にあり、どれも `EVAL_MOBILITY` になっている。
- usi.cpp の 125〜127 行目で `Eval::add_options` を呼んでいる。NNUE 版では evaluate_nnue.cpp の `add_options_` がこれにあたる。
- `OptionsMap::add` は、同じ名前が二度登録されると `exit` する（usioption.cpp 67 行目）。`HceWeightsFile` は KIKI 版にしか無いので、NNUE 版で登録しても衝突しない。

### 重みの精度

埋め込む配列は、fit の JSON ではなく `rank-contact.txt` から作る。

- txt は `.8g` で書き出されている。JSON と float32 で比べると、4474 個のうち 534 個が食い違う。JSON から作ると、埋め込みとファイルで explain の数字が変わる。
- アプリ側の `embed_hce_weights.py` の `hce_cpp` は `.6f` で書く。これでは txt から作っても 1520 個が変わり、一番小さい重み（6.1e-23）は 0 になる。
- txt の値を float32 で読み、`%.9g` で書けば、4474 個すべてが元に戻ることを確かめた。エンジンが使う `(float)strtod` とも一致する。

入力にする txt:

- 5 行の `#` ヘッダ（個数と各ブロックの大きさ、コーパス、target `value_q`、ridge alpha と行数、held-out RMSE 502.1）、空行、4474 個の値。
- 切片は無い。`layout_for_dim(4474)` は rank+contact、駒をまとめた版、kind 1 と判定する。
- sha256 は `2e89f5cb5dfbe32682532088132c9bd2a332b45e5cebc0d4513d5be42fdf9799`。

## 設計

### ファイル

| ファイル | 変更 |
|---|---|
| `eval/mobility/` → `eval/hce/` | ディレクトリごと改名。`evaluate_mobility.cpp` は `evaluate_hce.cpp` にする。`hce_mlp.h` と `mobility_weights_embedded.cpp` は名前を変えずに移す |
| `eval/hce/hce_explain.cpp` | 新規。explain の本体、重みの読み込み、`HceWeightsFile` の登録。全体を `USE_HCE_EXPLAIN` で囲む |
| `eval/hce/hce_explain_weights_embedded.cpp` | 新規（生成物）。rank+contact の 4474 個の配列 |
| `script/embed_hce_explain_weights.py` | 新規。txt から上の .cpp を作る |
| `eval/hce/evaluate_hce.cpp` | 改名前の 463〜648 行目（explain）を消す |
| `evaluate.h`、`usi.h`、`usi.cpp` | ガードを `USE_HCE_EXPLAIN` に替える。usi.cpp でオプションを登録する |
| `Makefile` | KIKI の `SOURCES` のパスを直す。`HCE_EXPLAIN` を足す |

`eval/hce/` には、KIKI 版だけでコンパイルするもの（`evaluate_hce.cpp`、`mobility_weights_embedded.cpp`）と、NNUE 版で `HCE_EXPLAIN=ON` のときだけコンパイルするもの（`hce_explain.cpp`、`hce_explain_weights_embedded.cpp`）が並ぶ。どれをどの edition に入れるかは Makefile が決める。

`evaluate_nnue.cpp` と `config.h` には手を入れない。

### hce_explain.cpp

evaluate_hce.cpp（改名前の evaluate_mobility.cpp）から持ってくるもの:

- `read_float_list` と `load_linear_weights`
  - `info string HceWeightsFile: ...` の文言は変えない。
- `effective_weights` の linear の分岐
  - kind 0、1、2（利きだけ、v2、段階で重み付け）の三つをそのまま持ってくる。ファイルで差し替えたときに、どの kind でも動くようにするため。
- `json_quote` から `emit_node` まで
- `hce_explain` の本体

KIKI の状態の代わりに、このファイルの中に重みの入れ物を一つ持つ。

```cpp
namespace YaneuraOu::Eval::Hce {
namespace {
struct LinearWeights {
	std::vector<float> w;
	int dim = 0;
	HumanLike::Layout layout;
	int kind = 1;
	double bias = 0.0;        // ファイルの末尾に切片があればその値
	const char* source = "none";
} g_w;
}
}
```

外に見せる関数は二つ。どちらも evaluate.h で `USE_HCE_EXPLAIN` の中で宣言する。

- `Eval::add_hce_explain_options(OptionsMap&)`: `HceWeightsFile` を登録する。
  - 値が空（GUI は `<empty>` を送り、それが `""` になる）なら、埋め込みの重みに戻す。
  - 読み込みに失敗したときも、埋め込みの重みに戻し、`info string HceWeightsFile: using embedded weights` と出す。
- `Eval::hce_explain(Position&, const std::vector<Move>&, int topn)`: 重みがまだ無ければ埋め込みの配列を読み、explain を出す。

`HceRoute`、`HceBias`、`HceTempo`、`HceMlpFile` は持ち込まない。

- 経路は linear しか無い。
- bias は定数なので、`d_*` と `top` では打ち消し合う。
- tempo は今の `emit_node` でも使われていない。

### explain の出力

ヘッダの項目は今の KIKI 版と同じにし、一つだけ足す。

- 残すもの: `route`（常に `"linear"`）、`variant`、`pieces`、`features`、`phase`、`pov`、`caveat`
- 足すもの: `"weights":"embedded"` または `"weights":"file"`
  - 重みファイルが読めなかったときは、黙って埋め込みに戻る。どちらで計算したかが出力にないとわからないので、この項目を足す。

各節点の行（`ply`、`move`、`sfen`、`see`、`gives_check`、`black_pov`、三つのブロックとその差分、`top`）は変えない。

`black_pov` は副評価器の値で、NNUE の評価値ではない。このことは `docs/manaka/hce-v2.md` に書き足す。

### usi.cpp

- 416 行目と 1421 行目のガード、および usi.h 201 行目のガードを `USE_HCE_EXPLAIN` にする。
- 127 行目の直後で、`#if defined(USE_HCE_EXPLAIN)` の中で `Eval::add_hce_explain_options(engine.get_options());` を呼ぶ。

### Makefile

KIKI の `SOURCES`（423〜424 行目）は、改名に合わせてパスを直す。

```make
		eval/hce/evaluate_hce.cpp                                              \
		eval/hce/mobility_weights_embedded.cpp                                 \
```

160 行目の `EVAL_EMBEDDING` の後ろ:

```make
# HCE v2 の線形評価を explain 専用に NNUE 系の edition へ組み込む。
# 探索からは呼ばないので、ON と OFF で bench の node 数は変わらない。
# 重み (rank+contact、4474 個) は eval/hce/hce_explain_weights_embedded.cpp に埋め込んである。
# HceWeightsFile で差し替えられる。
HCE_EXPLAIN = OFF
# HCE_EXPLAIN = ON
```

NNUE のブロックの閉じ（549 行目の `endif`）の手前、`EVAL_EMBEDDING` の `endif`（548 行目）の後ろ:

```make
	ifeq ($(HCE_EXPLAIN),ON)
		CPPFLAGS += -DUSE_HCE_EXPLAIN
		SOURCES  +=                                                            \
			eval/humanlike/humanlike_eval.cpp                                  \
			eval/hce/hce_explain.cpp                                           \
			eval/hce/hce_explain_weights_embedded.cpp
	endif
```

KIKI のブロックには何も足さない。

`-MMD -MP` はマクロの変化を追わない。同じ OBJDIR で ON と OFF を切り替えると古いオブジェクトが残るので、`make clean` するか OBJDIR を分けるよう、コメントと文書に書く。

### 生成器

`script/embed_hce_explain_weights.py` を fork に置く。fork の中で完結させ、アプリ側の `embed_hce_weights.py` には頼らない。

- 入力は txt。ヘッダの 5 行を見出しに写す。
- 値は `%.9g` で書き、float32 に戻して元の値と一致することを確かめる。
- 見出しには、入力の sha256 と、並び（`4474 floats = material 17 + mobility 4407 + king_safety 50 (rank+contact, folded)`）を書く。
- 出力は `#if defined(USE_HCE_EXPLAIN)` で囲み、`YaneuraOu::Eval::Hce` に置く。
  - `extern const int kEmbeddedHceWeightsSize = 4474;`
  - `extern const float kEmbeddedHceWeights[4474] = {...};`

## 手順

a71df8ac の上に積み、一手順を一コミットにする。

1. **`eval/mobility/` を `eval/hce/` に改名する。** 中身は変えない。
   - `git mv source/eval/mobility source/eval/hce`、続けて `git mv source/eval/hce/evaluate_mobility.cpp source/eval/hce/evaluate_hce.cpp`。
   - Makefile の KIKI の `SOURCES` と、humanlike_eval.cpp 467 行目のコメントを直す。
   - packages の README と SPEC、wasm_build.js のコメントのパスを `eval/hce/` に直す。すでに `eval/kiki/` と間違っていた 2 か所もここで直す。
   - 通る条件:
     - `git diff -M --stat` で、三つのファイルがどれも 100% の改名として出る。
     - KIKI 版がビルドでき、`bench` の `Nodes searched` が a71df8ac の KIKI 版と一致する。
2. **生成器と生成物を足す。** 手を入れていないので、この時点ではどのビルドにも影響しない。
3. **explain を NNUE 版に移す。**
   - `hce_explain.cpp` を足し、evaluate_hce.cpp から explain を消す。
   - usi.cpp、usi.h、evaluate.h を変える。
   - Makefile に `HCE_EXPLAIN` を足す。
   - 通る条件: 下の検証 1〜4。
4. **アプリ側を直す**（別リポジトリの別 PR）。サブモジュールをこの作業の後ろに進めるときに一緒に行う。
   - `explain_move.py`
     - `HceRoute` を送らない。
     - NNUE の評価関数フォルダを渡す `--eval-dir` を足す。
     - 268 行目の help にある「KIKI 版」を直す。
   - `docs/manaka/hce-v2.md` に、NNUE 版での使い方と `black_pov` の意味を書き足す。
   - 改名で古くなる参照を直す。
     - コードが実際に使うパス: `tideborn/tools/fit_hce.py` 79 行目（`.../eval/mobility/mobility_weights_embedded.cpp`）。v1 の係数との比較表に使う。
     - コメントと docstring: `fit_hce.py` 37 行目、`embed_hce_weights.py` 34・48 行目、`scripts/xcheck_linear_value.py` 7 行目、`scripts/hce_weights_file.py` 10 行目、`manaka-hce-fit.rs` 103・654 行目。
     - 文書: `docs/manaka/hce-v2.md` 124 行目（日本語版 111 行目）、`docs/manaka/analysis-protocol.md` 47・200・474〜477 行目（日本語版 40・184・433〜436 行目）。
       - analysis-protocol.md の `evaluate_mobility.cpp:689` や `:446` は、explain を消すと行番号もずれる。パスと一緒に行番号も引き直す。

## 検証

g++ 13.3、AVX2 の手元の機械で行う。構成ごとに OBJDIR と TARGETDIR を分ける。

```sh
# 埋めてから実行する
BASE=<a71df8ac を展開した worktree>
PR=<この作業の worktree>
KP256=<kp256 の評価関数フォルダ>
W=<rank-contact.txt>
OUT=<作業用の出力フォルダ>

B() { # B <ソースの根> <名前> <edition> [追加の引数...]
  mkdir -p "$OUT/bin/$2"
  make -C "$1/source" -j20 normal COMPILER=g++ YANEURAOU_EDITION="$3" \
       OBJDIR="$OUT/obj/$2" TARGETDIR="$OUT/bin/$2" "${@:4}"; }
B "$PR"   off-kp256 YANEURAOU_ENGINE_NNUE_KP256
B "$PR"   on-kp256  YANEURAOU_ENGINE_NNUE_KP256 HCE_EXPLAIN=ON
B "$PR"   on-nnue   YANEURAOU_ENGINE_NNUE HCE_EXPLAIN=ON
B "$BASE" kiki-base YANEURAOU_ENGINE_KIKI
B "$PR"   kiki-new  YANEURAOU_ENGINE_KIKI
```

### 1. 探索が変わらない

OFF と ON で `Nodes searched` が一致すること。bench の既定は時間制限なので、`depth` を明示する。

```sh
NB="usi\nsetoption name EvalDir value $KP256\nisready\nbench 64 1 13 default depth\nquit\n"
for b in off-kp256 on-kp256; do
  printf "$NB" | "$OUT/bin/$b/YaneuraOu-by-gcc" 2>&1 | grep 'Nodes searched'
done
grep -c HceWeightsFile "$OUT/bin/off-kp256/YaneuraOu-by-gcc"                                # 0
printf 'usi\nquit\n' | "$OUT/bin/on-kp256/YaneuraOu-by-gcc" | grep -c 'option name Hce'     # 1
```

### 2. explain の数字が KIKI 版と一致する

PR #35 の KIKI 版（`kiki-base`）を正解として使う。同じ重みファイルを渡すので、ヘッダの `weights` 以外は一致するはずである。

```sh
LINE='position startpos moves 7g7f 3c3d\nexplain top 3 moves 8h2b+ 3a2b\nquit\n'
printf "usi\nsetoption name HceWeightsFile value $W\n$LINE" \
  | "$OUT/bin/on-kp256/YaneuraOu-by-gcc" | grep '^{' | sed 's/,"weights":"[a-z]*"//' > "$OUT/nnue-file.jsonl"
printf "usi\nsetoption name HceWeightsFile value $W\nsetoption name HceRoute value linear\n$LINE" \
  | "$OUT/bin/kiki-base/YaneuraOu-by-gcc" | grep '^{' > "$OUT/kiki.jsonl"
cmp "$OUT/nnue-file.jsonl" "$OUT/kiki.jsonl" && wc -l "$OUT/kiki.jsonl"    # 4 行 (ヘッダと ply 0, 1, 2)
```

PR #35 で確かめた値とも照らし合わせる。

- ply 1 は material +1062、black_pov 879。
- ply 2 は see 945 で、material は 0 に戻る。
- 先頭の列は `material.hand.bishop`。

rank+contact 以外の幅の重み（plain、tapered の版など）でも同じ比較をして、kind 0 と kind 2 の分岐が移せていることを確かめる。

### 3. 埋め込みとファイルで同じ数字になる

```sh
printf "usi\n$LINE" | "$OUT/bin/on-kp256/YaneuraOu-by-gcc" | grep '^{' \
  | sed 's/,"weights":"[a-z]*"//' | cmp - "$OUT/nnue-file.jsonl"
# 既定の NNUE edition: isready を送らなければ、ネットワークが無くても explain は動くはず
printf "usi\n$LINE" | "$OUT/bin/on-nnue/YaneuraOu-by-gcc" | grep '^{' \
  | sed 's/,"weights":"[a-z]*"//' | cmp - "$OUT/nnue-file.jsonl"
```

`setoption name HceWeightsFile value <存在しないパス>` のあとでも explain が埋め込みの重みで動き、`"weights":"embedded"` と出ることも確かめる。

### 4. KIKI 版がビルドできる

`kiki-new` のビルドが通り、`usi` にこれまでどおり `HceRoute` と `HceWeightsFile` が出ること。explain は無くなっているので、`explain` は未知のコマンドになる。

`kiki-base` と `kiki-new` で `bench` の `Nodes searched` が一致することも確かめる。改名と explain の削除のどちらでも、KIKI 版の探索は変わらないはずである。

```sh
KB="usi\nisready\nbench 64 1 13 default depth\nquit\n"
for b in kiki-base kiki-new; do
  printf "$KB" | "$OUT/bin/$b/YaneuraOu-by-gcc" 2>&1 | grep 'Nodes searched'
done
```

### 5. サイズ

LTO が効いているので、最終的なバイナリで比べる。

```sh
stat -c '%n %s' "$OUT"/bin/{off,on}-kp256/YaneuraOu-by-gcc
```

見込みは数十 KB。配列が 17,896 バイトで、残りは特徴量の抽出と JSON の出力である。

### 片付け

worktree `$BASE` と `$OUT` を消す。

## ここでは確かめられないこと

- MSVC と vcxproj でのビルド。vcxproj には humanlike のファイルが無いので、Visual Studio で ON にするには別に足す必要がある。
- em++ と WASM。
- clang++。Makefile の既定のコンパイラで、CI もこれを使う。
- 既定の NNUE edition の bench。手元には HalfKP256 の `nn.bin` が無い。

## リスク

- **重みの精度。** txt を入力にし、`%.9g` で書くことで防ぐ。検証 3 で確かめる。
- **`-MMD` の古いオブジェクト。** 上に書いたとおり、文書に書いておく。
- **KIKI 版の explain が無くなる。** PR #35 の説明文とアプリ側の `hce-v2.md` は、explain を KIKI 版の機能として書いている。手順 4 でアプリ側の文書を直す。PR #35 がまだマージされていなければ、説明文も直す。
- **改名で古くなるパス。** fork の中は手順 1 ですべて直す。アプリ側は、サブモジュールがこの作業の後ろに進むまでは古いパスのままで正しいので、手順 4 でサブモジュールを進めるときに一緒に直す。`fit_hce.py` 79 行目だけはコードが使うパスである。v1 の利きの係数を読み、新しい係数の横に並べて表示するのに使う。ファイルが無いと `None` を返すだけなので（177〜178 行目）、直し忘れても落ちずに、比較の表が黙って消える。
- **今もある挙動で、この作業では変えないもの。**
  - usi.cpp は、指し手の列の途中に不正な手があると、黙ってそこで止まる。そのため `"illegal move"` は USI からは出ない。
  - explain は `engine.get_position()` を動かしてから戻す。
