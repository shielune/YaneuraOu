#!/usr/bin/env python3
"""
Mobility (MB) binpack → Ridge fit → MOBI weights.bin

binpack を直接受け取り、エンジンなしで特徴量抽出から学習・検証まで完結する。

必要ライブラリ:
    pip install cshogi scikit-learn numpy

Usage:
    # train.binpack のみ (内部で train/valid 分割)
    python scripts/train_mobility.py \\
        --binpack data/train.binpack \\
        --out eval/mobility/weights.bin

    # train / valid を別ファイルで指定
    python scripts/train_mobility.py \\
        --binpack data/train.binpack \\
        --valid   data/valid.binpack \\
        --out eval/mobility/weights.bin \\
        --alpha 1.0

binpack フォーマット: YaneuraOu PackedSfenValue (40 bytes/局面)
    [0:32]  packed sfen (HuffmanCodedPos)
    [32:34] int16  score  (side-to-move POV, centipawn)
    [34:36] uint16 move
    [36:38] uint16 gamePly
    [38]    int8   game_result
    [39]    uint8  padding
"""

import argparse
import struct
import sys

import numpy as np
from sklearn.linear_model import Ridge

try:
    from tqdm import tqdm
    HAS_TQDM = True
except ImportError:
    HAS_TQDM = False

try:
    import cshogi
    from cshogi import (
        BLACK, WHITE,
        PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK,
        PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER, HORSE, DRAGON, KING,
    )
except ImportError:
    sys.exit("cshogi が必要です:  pip install cshogi")

# ---------------------------------------------------------------------------
# 定数
# ---------------------------------------------------------------------------

NUM_BASE_BOARD = 107
NUM_BASE_HAND  = 7
NUM_BASE       = NUM_BASE_BOARD + NUM_BASE_HAND  # 114

PSV_SIZE = 40  # sizeof(PackedSfenValue)

# ---------------------------------------------------------------------------
# インデックステーブル (C++ init_index_tables() と等価)
# ---------------------------------------------------------------------------

def _build_index_tables():
    board_idx  = {}  # (pt, abs_df, dr_bpov) → int
    hand_pt_idx = {} # pt → int

    idx = 0

    def add_board(pt, df, dr):
        nonlocal idx
        key = (pt, abs(df), dr)
        if key not in board_idx:
            board_idx[key] = idx
            idx += 1

    def alias_board(pt, df, dr, target):
        key = (pt, abs(df), dr)
        if key not in board_idx:
            board_idx[key] = target

    # 歩
    add_board(PAWN, 0, -1)
    # 香: 前 1〜8
    for d in range(1, 9):
        add_board(LANCE, 0, -d)
    # 桂
    add_board(KNIGHT, 1, -2)
    # 銀: 前/前斜/後斜
    add_board(SILVER, 0, -1)
    add_board(SILVER, 1, -1)
    add_board(SILVER, 1,  1)
    # 金型 (成歩・成香・成桂・成銀は GOLD にエイリアス)
    for df, dr in [(0, -1), (1, -1), (1, 0), (0, 1)]:
        add_board(GOLD, df, dr)
        gid = board_idx[(GOLD, abs(df), dr)]
        for pt in [PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER]:
            alias_board(pt, df, dr, gid)
    # 玉: 8方向 → 左右対称で 5
    for df, dr in [(0,-1),(1,-1),(1,0),(1,1),(0,1)]:
        add_board(KING, df, dr)
    # 角: 斜め × 8距離
    for d in range(1, 9):
        add_board(BISHOP, d, -d)
        add_board(BISHOP, d,  d)
    # 飛: 前後横 × 8距離
    for d in range(1, 9):
        add_board(ROOK, 0, -d)
        add_board(ROOK, 0,  d)
        add_board(ROOK, d,  0)
    # 馬: 角16 + 直交隣接3
    for d in range(1, 9):
        add_board(HORSE, d, -d)
        add_board(HORSE, d,  d)
    add_board(HORSE, 0, -1)
    add_board(HORSE, 0,  1)
    add_board(HORSE, 1,  0)
    # 龍: 飛24 + 斜め隣接2
    for d in range(1, 9):
        add_board(DRAGON, 0, -d)
        add_board(DRAGON, 0,  d)
        add_board(DRAGON, d,  0)
    add_board(DRAGON, 1, -1)
    add_board(DRAGON, 1,  1)

    assert idx == NUM_BASE_BOARD, f"board index mismatch: {idx} != {NUM_BASE_BOARD}"

    # 持ち駒: 駒種 × 1次元
    for pt in [PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK]:
        hand_pt_idx[pt] = idx
        idx += 1

    assert idx == NUM_BASE, f"total index mismatch: {idx} != {NUM_BASE}"
    return board_idx, hand_pt_idx


BOARD_IDX, HAND_IDX = _build_index_tables()

# ---------------------------------------------------------------------------
# 盤面ユーティリティ
# ---------------------------------------------------------------------------

def file_of(sq): return sq // 9   # 0=9筋(右端), 8=1筋(左端)
def rank_of(sq): return sq % 9    # 0=1段(上端), 8=9段(下端)
def make_sq(f, r): return f * 9 + r
def in_board(f, r): return 0 <= f < 9 and 0 <= r < 9


def _attacks(pt, color, sq, occ):
    """
    駒 (pt, color) が sq に置かれたときの利き升リストを返す。
    occ: set[int] — 占有升の集合 (スライダーのブロック判定に使用)

    Returns list of (abs_df, dr_bpov):
        abs_df  = |file(target) - file(source)|
        dr_bpov = rank差を先手視点に正規化
                  BLACK: そのまま / WHITE: 符号反転
    """
    f0, r0 = file_of(sq), rank_of(sq)
    fwd    = -1 if color == BLACK else 1   # 前方の rank 増分 (絶対座標)
    result = []

    def emit(f, r):
        dr_abs = r - r0
        result.append((abs(f - f0), dr_abs if color == BLACK else -dr_abs))
        return make_sq(f, r)

    def ray(df, dr):
        f, r = f0 + df, r0 + dr
        while in_board(f, r):
            ts = emit(f, r)
            if ts in occ:
                break
            f += df; r += dr

    def step(df, dr):
        f, r = f0 + df, r0 + dr
        if in_board(f, r):
            emit(f, r)

    if pt == PAWN:
        step(0, fwd)
    elif pt == LANCE:
        ray(0, fwd)
    elif pt == KNIGHT:
        for df in (-1, 1):
            step(df, fwd * 2)
    elif pt == SILVER:
        for df in (-1, 0, 1): step(df, fwd)
        for df in (-1, 1):    step(df, -fwd)
    elif pt in (GOLD, PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER):
        for df in (-1, 0, 1): step(df, fwd)
        for df in (-1, 1):    step(df, 0)
        step(0, -fwd)
    elif pt == BISHOP:
        for df, dr in ((1,1),(1,-1),(-1,1),(-1,-1)): ray(df, dr)
    elif pt == ROOK:
        for df, dr in ((0,1),(0,-1),(1,0),(-1,0)):   ray(df, dr)
    elif pt == HORSE:
        for df, dr in ((1,1),(1,-1),(-1,1),(-1,-1)): ray(df, dr)
        for df, dr in ((0,1),(0,-1),(1,0),(-1,0)):   step(df, dr)
    elif pt == DRAGON:
        for df, dr in ((0,1),(0,-1),(1,0),(-1,0)):   ray(df, dr)
        for df, dr in ((1,1),(1,-1),(-1,1),(-1,-1)): step(df, dr)
    elif pt == KING:
        for df in (-1, 0, 1):
            for dr in (-1, 0, 1):
                if df or dr: step(df, dr)

    return result


def _drop_legal_rank(color, pt, r):
    """打ち禁段チェック (歩香は1段目不可, 桂は1・2段目不可)"""
    rel = r if color == BLACK else (8 - r)
    if pt in (PAWN, LANCE): return rel != 0
    if pt == KNIGHT:        return rel not in (0, 1)
    return True

# ---------------------------------------------------------------------------
# 114 次元特徴量抽出 (C++ extract_features() と等価)
# ---------------------------------------------------------------------------

def extract_features(board):
    """
    cshogi.Board → np.ndarray shape (114,) dtype=float32

    board.piece(sq)         : 0 = 空升
    cshogi.piece_to_color() : BLACK=0 / WHITE=1
    cshogi.piece_type()     : PAWN=1 .. DRAGON=14
    board.pieces_in_hand[color][pt] : 持ち駒枚数
    """
    feat = np.zeros(NUM_BASE, dtype=np.float32)
    occ  = {sq for sq in range(81) if board.piece(sq) != 0}

    # ---- 盤上駒 ----
    for sq in range(81):
        pc = board.piece(sq)
        if pc == 0:
            continue
        color = cshogi.piece_to_color(pc)
        pt    = cshogi.piece_type(pc)
        sign  = 1.0 if color == BLACK else -1.0
        for abs_df, dr in _attacks(pt, color, sq, occ):
            fi = BOARD_IDX.get((pt, abs_df, dr))
            if fi is not None:
                feat[fi] += sign

    # ---- 持ち駒 ----
    for color in (BLACK, WHITE):
        sign = 1.0 if color == BLACK else -1.0
        hand = board.pieces_in_hand[color]

        for pt in (PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK):
            cnt = int(hand[pt])
            if cnt == 0:
                continue
            fi = HAND_IDX[pt]

            # 二歩: その筋に既に自分の歩がある升には打てない
            pawn_files: set = set()
            if pt == PAWN:
                pawn_files = {
                    file_of(sq) for sq in range(81)
                    if board.piece(sq) != 0
                    and cshogi.piece_to_color(board.piece(sq)) == color
                    and cshogi.piece_type(board.piece(sq)) == PAWN
                }

            # 全合法打ち升 → 総利きマス数 × 枚数 を集計
            for sq in range(81):
                if sq in occ:
                    continue
                r = rank_of(sq)
                if not _drop_legal_rank(color, pt, r):
                    continue
                if pt == PAWN and file_of(sq) in pawn_files:
                    continue
                feat[fi] += sign * cnt * len(_attacks(pt, color, sq, occ))

    return feat

# ---------------------------------------------------------------------------
# binpack 読み込み + 特徴量行列構築
# ---------------------------------------------------------------------------

def load_dataset(path, max_positions, label=""):
    import os
    board  = cshogi.Board()
    feats, scores = [], []
    skip = 0

    # ファイルサイズから総局面数を推定してプログレスバーに使う
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
            # BLACK POV に統一 (WHITE 手番なら符号反転)
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
    ap.add_argument("--binpack",       required=True,              help="学習用 binpack")
    ap.add_argument("--valid",         default=None,               help="検証用 binpack (省略時は --train-ratio で内部分割)")
    ap.add_argument("--out",           required=True,              help="出力 weights.bin (MOBI 形式)")
    ap.add_argument("--max-positions", type=int, default=1_000_000, help="学習局面の上限")
    ap.add_argument("--max-valid",     type=int, default=200_000,   help="検証局面の上限 (--valid 指定時)")
    ap.add_argument("--train-ratio",   type=float, default=0.9,    help="内部分割時の学習比率")
    ap.add_argument("--alpha",         type=float, default=1.0,    help="Ridge 正則化係数")
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
    print(f"\nfitting Ridge  alpha={args.alpha}  N_train={len(y_tr):,}  D={NUM_BASE} ...")
    import time
    t0 = time.time()
    model = Ridge(alpha=args.alpha, fit_intercept=False)
    model.fit(X_tr, y_tr)
    print(f"done in {time.time() - t0:.1f}s")
    w = model.coef_  # shape (114,)

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

    # 持ち駒の重み確認
    hand_names = ["歩(持)", "香(持)", "桂(持)", "銀(持)", "金(持)", "角(持)", "飛(持)"]
    print("\n持ち駒重み:")
    for name, fi in zip(hand_names, range(107, 114)):
        print(f"  [{fi}] {name}: {w[fi]:.4f}")

    # ---- MOBI バイナリ保存 ----
    w32 = w.astype(np.float32)
    with open(args.out, "wb") as f:
        f.write(b"MOBI")
        f.write(struct.pack("<I", len(w32)))
        f.write(struct.pack(f"<{len(w32)}f", *w32))

    size = 4 + 4 + len(w32) * 4
    print(f"\nwrote {len(w32)} weights → {args.out}  ({size} bytes, MOBI format)")


if __name__ == "__main__":
    main()
