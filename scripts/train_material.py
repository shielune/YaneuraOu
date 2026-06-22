#!/usr/bin/env python3
"""
Material (MAT) binpack → Ridge fit → MATW weights.bin

binpack を直接受け取り、エンジンなしで特徴量抽出から学習・検証まで完結する。
利き計算は不要で駒の枚数を数えるだけなので非常に高速。

必要ライブラリ:
    pip install cshogi scikit-learn numpy tqdm

Usage:
    # train / valid を別ファイルで指定
    python scripts/train_material.py \\
        --binpack data/train.binpack \\
        --valid   data/valid.binpack \\
        --out     eval/material/weights.bin \\
        --alpha   1.0

    # valid なしで内部分割 (train 90% / valid 10%)
    python scripts/train_material.py \\
        --binpack data/train.binpack \\
        --out     eval/material/weights.bin

特徴量レイアウト (17次元):
    [0]     歩(盤)
    [1]     香(盤)
    [2]     桂(盤)
    [3]     銀(盤)
    [4]     金(盤)
    [5]     角(盤)
    [6]     飛(盤)
    [7]     金型成駒(盤) — 成歩/成香/成桂/成銀 まとめて
    [8]     馬(盤)
    [9]     龍(盤)
    [10]    歩(持)
    [11]    香(持)
    [12]    桂(持)
    [13]    銀(持)
    [14]    金(持)
    [15]    角(持)
    [16]    飛(持)
"""

import argparse
import os
import struct
import sys
import time

import numpy as np
from sklearn.linear_model import Ridge

try:
    import cshogi
    from cshogi import (
        BLACK, WHITE,
        PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK,
        PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER, HORSE, DRAGON,
    )
except ImportError:
    sys.exit("cshogi が必要です:  pip install cshogi")

try:
    from tqdm import tqdm
    HAS_TQDM = True
except ImportError:
    HAS_TQDM = False

# ---------------------------------------------------------------------------
# 定数
# ---------------------------------------------------------------------------

NUM_FEATURES = 17
PSV_SIZE     = 40  # sizeof(PackedSfenValue)

# 盤上駒種 → index
BOARD_INDEX = {
    PAWN:       0,
    LANCE:      1,
    KNIGHT:     2,
    SILVER:     3,
    GOLD:       4,
    BISHOP:     5,
    ROOK:       6,
    PRO_PAWN:   7,  # 金型成駒まとめて
    PRO_LANCE:  7,
    PRO_KNIGHT: 7,
    PRO_SILVER: 7,
    HORSE:      8,
    DRAGON:     9,
}

# 持ち駒種 → index
HAND_INDEX = {
    PAWN:   10,
    LANCE:  11,
    KNIGHT: 12,
    SILVER: 13,
    GOLD:   14,
    BISHOP: 15,
    ROOK:   16,
}

FEATURE_NAMES = [
    "歩(盤)", "香(盤)", "桂(盤)", "銀(盤)", "金(盤)", "角(盤)", "飛(盤)",
    "金型成駒(盤)", "馬(盤)", "龍(盤)",
    "歩(持)", "香(持)", "桂(持)", "銀(持)", "金(持)", "角(持)", "飛(持)",
]

# ---------------------------------------------------------------------------
# 特徴量抽出 (駒の枚数を数えるだけ、利き計算不要)
# ---------------------------------------------------------------------------

def extract_features(board):
    """
    cshogi.Board → np.ndarray shape (17,) dtype=float32  (先手視点・先手プラス)
    """
    feat = np.zeros(NUM_FEATURES, dtype=np.float32)

    # 盤上駒
    for sq in range(81):
        pc = board.piece(sq)
        if pc == 0:
            continue
        color = cshogi.piece_to_color(pc)
        pt    = cshogi.piece_type(pc)
        fi    = BOARD_INDEX.get(pt)
        if fi is None:  # KING は除外
            continue
        feat[fi] += 1.0 if color == BLACK else -1.0

    # 持ち駒
    for color in (BLACK, WHITE):
        sign = 1.0 if color == BLACK else -1.0
        hand = board.pieces_in_hand[color]
        for pt, fi in HAND_INDEX.items():
            cnt = int(hand[pt])
            if cnt:
                feat[fi] += sign * cnt

    return feat

# ---------------------------------------------------------------------------
# binpack 読み込み + 特徴量行列構築
# ---------------------------------------------------------------------------

def load_dataset(path, max_positions, label=""):
    board = cshogi.Board()
    feats, scores = [], []
    skip = 0

    total_in_file = os.path.getsize(path) // PSV_SIZE
    total = min(max_positions, total_in_file)

    if HAS_TQDM:
        bar = tqdm(total=total, desc=f"load {label}", unit="pos",
                   dynamic_ncols=True, leave=True)
    else:
        bar = None

    with open(path, "rb") as f:
        for i in range(max_positions):
            data = f.read(PSV_SIZE)
            if len(data) < PSV_SIZE:
                break
            sfen_bytes = data[:32]
            score_stm  = struct.unpack_from("<h", data, 32)[0]  # side-to-move POV

            try:
                board.set_psfen(sfen_bytes)
            except Exception:
                skip += 1
                if bar: bar.update(1)
                continue

            feats.append(extract_features(board))
            score_black = float(score_stm) if board.turn == BLACK else float(-score_stm)
            scores.append(score_black)

            if bar:
                bar.update(1)
            elif (i + 1) % 100_000 == 0:
                print(f"  [{label}] {i+1:,} ...", flush=True)

    if bar:
        bar.close()
    if skip:
        print(f"  [{label}] skipped {skip} records (parse error)")

    X = np.stack(feats).astype(np.float32)
    y = np.array(scores, dtype=np.float64)
    print(f"  [{label}] {len(y):,} positions loaded  X={X.shape}")
    return X, y

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--binpack",       required=True,               help="学習用 binpack")
    ap.add_argument("--valid",         default=None,                help="検証用 binpack (省略時は --train-ratio で内部分割)")
    ap.add_argument("--out",           required=True,               help="出力 weights.bin (MATW 形式)")
    ap.add_argument("--max-positions", type=int, default=1_000_000, help="学習局面の上限")
    ap.add_argument("--max-valid",     type=int, default=200_000,   help="検証局面の上限 (--valid 指定時)")
    ap.add_argument("--train-ratio",   type=float, default=0.9,     help="内部分割時の学習比率")
    ap.add_argument("--alpha",         type=float, default=1.0,     help="Ridge 正則化係数")
    ap.add_argument("--seed",          type=int,   default=42)
    args = ap.parse_args()

    # ---- データ読み込み ----
    print(f"loading train: {args.binpack}")
    X, y = load_dataset(args.binpack, args.max_positions, label="train")

    if args.valid:
        X_tr, y_tr = X, y
        print(f"\nloading valid: {args.valid}")
        X_va, y_va = load_dataset(args.valid, args.max_valid, label="valid")
    else:
        rng  = np.random.default_rng(args.seed)
        perm = rng.permutation(len(y))
        n_tr = int(len(y) * args.train_ratio)
        X_tr, y_tr = X[perm[:n_tr]], y[perm[:n_tr]]
        X_va, y_va = X[perm[n_tr:]], y[perm[n_tr:]]

    print(f"\ntrain: {len(y_tr):,}  valid: {len(y_va):,}")
    print(f"y_train: mean={y_tr.mean():.1f}  std={y_tr.std():.1f}  "
          f"p5={np.percentile(y_tr, 5):.0f}  p95={np.percentile(y_tr, 95):.0f}")

    # ---- Ridge fit ----
    print(f"\nfitting Ridge  alpha={args.alpha}  N_train={len(y_tr):,}  D={NUM_FEATURES} ...")
    t0 = time.time()
    model = Ridge(alpha=args.alpha, fit_intercept=False)
    model.fit(X_tr, y_tr)
    print(f"done in {time.time() - t0:.1f}s")
    w = model.coef_  # shape (17,)

    # ---- 評価 ----
    def metrics(Xm, ym):
        pred   = Xm @ w
        res    = ym - pred
        rmse   = float(np.sqrt((res ** 2).mean()))
        ss_res = float((res ** 2).sum())
        ss_tot = float(((ym - ym.mean()) ** 2).sum())
        r2     = 1.0 - ss_res / ss_tot if ss_tot > 0 else float("nan")
        return rmse, r2

    tr_rmse, tr_r2 = metrics(X_tr, y_tr)
    va_rmse, va_r2 = metrics(X_va, y_va)

    print(f"\ntrain RMSE = {tr_rmse:.2f} cp   R² = {tr_r2:.4f}")
    print(f"valid RMSE = {va_rmse:.2f} cp   R² = {va_r2:.4f}")
    print(f"weights  : min={w.min():.3f}  max={w.max():.3f}  |mean|={np.abs(w).mean():.3f}")

    # 全重み表示
    print("\n重み一覧:")
    for i, (name, wi) in enumerate(zip(FEATURE_NAMES, w)):
        print(f"  [{i:2d}] {name}: {wi:.4f}")

    # ---- MATW バイナリ保存 ----
    w32 = w.astype(np.float32)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(b"MATW")
        f.write(struct.pack("<I", len(w32)))
        f.write(struct.pack(f"<{len(w32)}f", *w32))

    size = 4 + 4 + len(w32) * 4
    print(f"\nwrote {len(w32)} weights → {args.out}  ({size} bytes, MATW format)")


if __name__ == "__main__":
    main()
