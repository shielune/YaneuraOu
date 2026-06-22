#!/usr/bin/env python3
"""
Mobility (MB) Ridge fit。バリアント 1〜4 に対応。

Usage:
    python scripts/fit_mobility.py \
        --x /data/feat.X.bin \
        --y /data/feat.y.bin \
        --variant 1 \
        --alpha 1.0 \
        --out eval/mobility/weights.bin

バリアントと次元数:
    1 : 114          (駒種 × 方向 × 距離、盤上107 + 持ち駒7)
    2 : 2,394        (+ 利き先の駒種 21種)
    3 : 9,234        (+ 攻撃駒の升 81)
    4 : 194,103      (+ 両方)

出力フォーマット:
    .bin  → MOBI バイナリ (magic + uint32 dim + float32[])
    .txt  → テキスト (1行1値、コメント行 # あり)
"""
import argparse
import struct
import numpy as np
from sklearn.linear_model import Ridge

NUM_BASE = 114  # 盤上107 + 持ち駒7
VARIANT_DIMS = {1: NUM_BASE, 2: NUM_BASE * 21, 3: NUM_BASE * 81, 4: NUM_BASE * 21 * 81}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--x",       required=True, help="X.bin (float32 [N, D])")
    p.add_argument("--y",       required=True, help="y.bin (float32 [N], BLACK POV)")
    p.add_argument("--variant", type=int, default=1, choices=[1, 2, 3, 4])
    p.add_argument("--alpha",   type=float, default=1.0, help="Ridge 正則化係数")
    p.add_argument("--out",     required=True, help="出力 weights.txt")
    args = p.parse_args()

    D = VARIANT_DIMS[args.variant]
    y_raw = np.frombuffer(open(args.y, "rb").read(), dtype=np.float32)
    N = len(y_raw)
    X = np.frombuffer(open(args.x, "rb").read(), dtype=np.float32).reshape(N, D)
    y = y_raw.astype(np.float64)

    print(f"variant={args.variant}  N={N}  D={D}  alpha={args.alpha}")
    print(f"y: mean={y.mean():.1f}  std={y.std():.1f}  "
          f"p5={np.percentile(y, 5):.0f}  p95={np.percentile(y, 95):.0f}")

    model = Ridge(alpha=args.alpha, fit_intercept=False)
    model.fit(X, y)

    w = model.coef_
    pred = X @ w
    rmse = float(np.sqrt(((y - pred) ** 2).mean()))
    print(f"train RMSE = {rmse:.2f} cp")
    print(f"weights: min={w.min():.3f}  max={w.max():.3f}  |mean|={np.abs(w).mean():.3f}")

    if args.out.endswith(".bin"):
        with open(args.out, "wb") as f:
            f.write(b"MOBI")
            f.write(struct.pack("<I", len(w)))
            f.write(struct.pack(f"<{len(w)}f", *w.astype(np.float32)))
        print(f"wrote {len(w)} weights (MOBI binary) → {args.out}")
    else:
        with open(args.out, "w") as f:
            f.write(f"# variant={args.variant}  alpha={args.alpha}  N={N}  D={D}  RMSE={rmse:.2f}\n")
            for v in w:
                f.write(f"{v:.6f}\n")
        print(f"wrote {len(w)} weights (text) → {args.out}")


if __name__ == "__main__":
    main()
