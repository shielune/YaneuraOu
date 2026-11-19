#!/usr/bin/env python3
"""
YaneuraOu-Material / YaneuraOu-Mobility のエンジン eval 出力を
学習済み weights から Python で再計算した期待値と照合する。

使い方:
    python scripts/test_eval.py \\
        --material-engine engine/arm64/YaneuraOu-Material-NoEval \\
        --mobility-engine engine/arm64/YaneuraOu-Mobility-NoEval

エンジンに `position ... / eval` を流して `eval = N` を取得、
Python 側で同じ局面を仕様書通りに計算して比較。±1cp 以内なら PASS。
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

try:
    import cshogi
    from cshogi import (
        BLACK, WHITE,
        PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK,
        PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER, HORSE, DRAGON,
    )
except ImportError:
    sys.exit("cshogi が必要:  pip install cshogi")

# ---------------------------------------------------------------------------
# weights ファイルパーサ
# ---------------------------------------------------------------------------

def load_weights(path, expect_magic, expect_dim):
    with open(path, "rb") as f:
        data = f.read()
    magic = data[:4]
    if magic != expect_magic:
        raise RuntimeError(f"{path}: bad magic {magic!r}, expected {expect_magic!r}")
    dim = struct.unpack("<I", data[4:8])[0]
    if dim != expect_dim:
        raise RuntimeError(f"{path}: dim {dim} != {expect_dim}")
    return list(struct.unpack(f"<{dim}f", data[8:]))

# ---------------------------------------------------------------------------
# Material expected (17 dim, train_material.py と同じ式)
# ---------------------------------------------------------------------------

MAT_BOARD_INDEX = {
    PAWN: 0, LANCE: 1, KNIGHT: 2, SILVER: 3, GOLD: 4, BISHOP: 5, ROOK: 6,
    PRO_PAWN: 7, PRO_LANCE: 7, PRO_KNIGHT: 7, PRO_SILVER: 7,
    HORSE: 8, DRAGON: 9,
}
MAT_HAND_INDEX = {PAWN: 10, LANCE: 11, KNIGHT: 12, SILVER: 13, GOLD: 14, BISHOP: 15, ROOK: 16}

def material_expected(board, w):
    """エンジンの返す値 (stm POV) を予測"""
    score = 0.0
    for sq in range(81):
        pc = board.piece(sq)
        if pc == 0:
            continue
        color = cshogi.piece_to_color(pc)
        pt    = cshogi.piece_type(pc)
        idx = MAT_BOARD_INDEX.get(pt)
        if idx is None:
            continue
        sign = 1.0 if color == BLACK else -1.0
        score += sign * w[idx]
    for color in (BLACK, WHITE):
        sign = 1.0 if color == BLACK else -1.0
        hand = board.pieces_in_hand[color]
        for pt, idx in MAT_HAND_INDEX.items():
            cnt = int(hand[pt])
            if cnt:
                score += sign * cnt * w[idx]
    score_black = int(score)
    return score_black if board.turn == BLACK else -score_black

# ---------------------------------------------------------------------------
# Mobility expected (114 dim) — train_mobility.py の extract_features と同じ
# ---------------------------------------------------------------------------

NUM_BASE = 114

def _build_mobility_tables():
    board_idx, hand_pt_idx = {}, {}
    idx = 0
    def add(pt, df, dr):
        nonlocal idx
        k = (pt, abs(df), dr)
        if k not in board_idx:
            board_idx[k] = idx; idx += 1
    def alias(pt, df, dr, target):
        board_idx[(pt, abs(df), dr)] = target

    add(PAWN, 0, -1)
    for d in range(1, 9): add(LANCE, 0, -d)
    add(KNIGHT, 1, -2)
    add(SILVER, 0, -1); add(SILVER, 1, -1); add(SILVER, 1, 1)
    for df, dr in [(0,-1),(1,-1),(1,0),(0,1)]:
        add(GOLD, df, dr)
        gid = board_idx[(GOLD, abs(df), dr)]
        for pt in [PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER]:
            alias(pt, df, dr, gid)
    for df, dr in [(0,-1),(1,-1),(1,0),(1,1),(0,1)]:
        # KING: cshogi の定数取得が手間なので 14 をハード
        k = (14, abs(df), dr)
        if k not in board_idx:
            board_idx[k] = idx; idx += 1
    for d in range(1, 9):
        add(BISHOP, d, -d); add(BISHOP, d, d)
    for d in range(1, 9):
        add(ROOK, 0, -d); add(ROOK, 0, d); add(ROOK, d, 0)
    for d in range(1, 9):
        add(HORSE, d, -d); add(HORSE, d, d)
    add(HORSE, 0, -1); add(HORSE, 0, 1); add(HORSE, 1, 0)
    for d in range(1, 9):
        add(DRAGON, 0, -d); add(DRAGON, 0, d); add(DRAGON, d, 0)
    add(DRAGON, 1, -1); add(DRAGON, 1, 1)

    assert idx == 107, f"board idx {idx} != 107"
    for pt in [PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK]:
        hand_pt_idx[pt] = idx; idx += 1
    assert idx == NUM_BASE
    return board_idx, hand_pt_idx

MOB_BOARD, MOB_HAND = _build_mobility_tables()

def file_of(sq): return sq // 9
def rank_of(sq): return sq % 9
def in_board(f, r): return 0 <= f < 9 and 0 <= r < 9

def _attacks(pt, color, sq, occ):
    f0, r0 = file_of(sq), rank_of(sq)
    fwd = -1 if color == BLACK else 1
    result = []
    def emit(f, r):
        dr_abs = r - r0
        result.append((abs(f - f0), dr_abs if color == BLACK else -dr_abs))
        return f * 9 + r
    def ray(df, dr):
        f, r = f0 + df, r0 + dr
        while in_board(f, r):
            ts = emit(f, r)
            if ts in occ: break
            f += df; r += dr
    def step(df, dr):
        f, r = f0 + df, r0 + dr
        if in_board(f, r): emit(f, r)

    if pt == PAWN: step(0, fwd)
    elif pt == LANCE: ray(0, fwd)
    elif pt == KNIGHT:
        for df in (-1, 1): step(df, fwd * 2)
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
    elif pt == 14:  # KING
        for df in (-1, 0, 1):
            for dr in (-1, 0, 1):
                if df or dr: step(df, dr)
    return result

def _drop_legal_rank(color, pt, r):
    rel = r if color == BLACK else (8 - r)
    if pt in (PAWN, LANCE): return rel != 0
    if pt == KNIGHT:        return rel not in (0, 1)
    return True

def mobility_expected(board, w):
    feat = [0.0] * NUM_BASE
    occ = {sq for sq in range(81) if board.piece(sq) != 0}

    # 盤上駒
    for sq in range(81):
        pc = board.piece(sq)
        if pc == 0: continue
        color = cshogi.piece_to_color(pc)
        pt    = cshogi.piece_type(pc)
        sign = 1.0 if color == BLACK else -1.0
        for abs_df, dr in _attacks(pt, color, sq, occ):
            fi = MOB_BOARD.get((pt, abs_df, dr))
            if fi is not None:
                feat[fi] += sign

    # 持ち駒
    for color in (BLACK, WHITE):
        sign = 1.0 if color == BLACK else -1.0
        hand = board.pieces_in_hand[color]
        for pt in (PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK):
            cnt = int(hand[pt])
            if cnt == 0: continue
            fi = MOB_HAND[pt]
            pawn_files = set()
            if pt == PAWN:
                pawn_files = {
                    file_of(sq) for sq in range(81)
                    if board.piece(sq) != 0
                    and cshogi.piece_to_color(board.piece(sq)) == color
                    and cshogi.piece_type(board.piece(sq)) == PAWN
                }
            for sq in range(81):
                if sq in occ: continue
                r = rank_of(sq)
                if not _drop_legal_rank(color, pt, r): continue
                if pt == PAWN and file_of(sq) in pawn_files: continue
                feat[fi] += sign * cnt * len(_attacks(pt, color, sq, occ))

    score = sum(feat[i] * w[i] for i in range(NUM_BASE))
    # エンジン側は ±3000 でクランプしている
    if score > 3000:  score = 3000
    if score < -3000: score = -3000
    score_black = int(score)
    return score_black if board.turn == BLACK else -score_black

# ---------------------------------------------------------------------------
# エンジン起動・eval 取得
# ---------------------------------------------------------------------------

def get_engine_eval(engine_path, eval_dir, sfen):
    cmds = (
        "usi\n"
        f"setoption name EvalDir value {eval_dir}\n"
        "setoption name USI_Hash value 1\n"
        "isready\n"
        f"position sfen {sfen}\n"
        "eval\n"
        "quit\n"
    )
    p = subprocess.run(
        [engine_path], input=cmds, capture_output=True, text=True, timeout=15
    )
    m = re.search(r"eval\s*=\s*(-?\d+)", p.stdout)
    if not m:
        raise RuntimeError(f"no eval in output:\n{p.stdout[-400:]}\n--- stderr ---\n{p.stderr[-400:]}")
    return int(m.group(1))

# ---------------------------------------------------------------------------
# テストケース定義
# ---------------------------------------------------------------------------

STARTPOS = "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1"

# 仕様書のテストケース + 追加で枚数倍テスト
CASES = [
    ("startpos b",          STARTPOS),
    ("startpos w",          STARTPOS.replace(" b ", " w ")),
    # BLACK 持ち駒 歩1
    ("B+1pawn b",  "lnsgkgsnl/1r5b1/pppppppp1/9/9/9/PPPPPPPP1/1B5R1/LNSGKGSNL b P 1"),
    ("B+1pawn w",  "lnsgkgsnl/1r5b1/pppppppp1/9/9/9/PPPPPPPP1/1B5R1/LNSGKGSNL w P 1"),
    # BLACK 持ち駒 歩2/3
    ("B+2pawn b",  "lnsgkgsnl/1r5b1/ppppppp2/9/9/9/PPPPPPP2/1B5R1/LNSGKGSNL b 2P 1"),
    ("B+3pawn b",  "lnsgkgsnl/1r5b1/pppppp3/9/9/9/PPPPPP3/1B5R1/LNSGKGSNL b 3P 1"),
    # 飛角入れ替えで成駒テスト
    ("B+dragon b", "lnsgkgsnl/1+R5b1/ppppppppp/9/9/9/PPPPPPPPP/7B1/LNSGKGSNL b - 1"),
    ("B+horse b",  "lnsgkgsnl/+B6b1/ppppppppp/9/9/9/PPPPPPPPP/7R1/LNSGKGSNL b - 1"),
]

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def run_suite(engine_path, weights_path, magic, dim, expected_fn, label):
    print(f"\n=== {label} ===")
    print(f"engine : {engine_path}")
    print(f"weights: {weights_path}")
    w = load_weights(weights_path, magic, dim)

    with tempfile.TemporaryDirectory() as eval_dir:
        # エンジンは EvalDir/weights.bin を読む
        import shutil
        shutil.copy(weights_path, os.path.join(eval_dir, "weights.bin"))

        fails = 0
        for name, sfen in CASES:
            board = cshogi.Board()
            board.set_sfen(sfen)
            exp = expected_fn(board, w)
            try:
                got = get_engine_eval(engine_path, eval_dir, sfen)
            except Exception as e:
                print(f"  [ERROR] {name}: {e}")
                fails += 1
                continue
            diff = got - exp
            ok = abs(diff) <= 1
            mark = "✓" if ok else "✗"
            print(f"  {mark} {name:12s}  expect={exp:+6d}  engine={got:+6d}  diff={diff:+d}")
            if not ok:
                fails += 1
    return fails

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--material-engine", default="engine/arm64/YaneuraOu-Material-NoEval")
    ap.add_argument("--mobility-engine", default="engine/arm64/YaneuraOu-Mobility-NoEval")
    ap.add_argument("--material-weights", default="eval/material/weights.bin")
    ap.add_argument("--mobility-weights", default="eval/mobility/weights.bin")
    args = ap.parse_args()

    fails = 0
    if os.path.exists(args.material_engine):
        fails += run_suite(args.material_engine, args.material_weights,
                           b"MATW", 17, material_expected, "Material")
    else:
        print(f"skip Material: {args.material_engine} not found")

    if os.path.exists(args.mobility_engine):
        fails += run_suite(args.mobility_engine, args.mobility_weights,
                           b"MOBI", 114, mobility_expected, "Mobility")
    else:
        print(f"skip Mobility: {args.mobility_engine} not found")

    print(f"\n{'-' * 40}")
    if fails == 0:
        print("✓ all tests passed")
        sys.exit(0)
    else:
        print(f"✗ {fails} test(s) failed")
        sys.exit(1)

if __name__ == "__main__":
    main()
