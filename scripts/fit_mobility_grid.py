#!/usr/bin/env python3
"""
alpha グリッド × variant の総当たり Ridge fit。

Usage:
    python scripts/fit_mobility_grid.py \
        --x    /data/feat_v{variant}.X.bin \
        --y    /data/feat_v{variant}.y.bin \
        --variants 1 2 3 4 \
        --alphas   0.1 0.3 1.0 3.0 10.0 \
        --out_dir  /data/grid_results

  --x / --y のパスに {variant} プレースホルダーが含まれる場合、各バリアントのファイルを自動解決する。
  プレースホルダーがない場合、全バリアントで同じファイルを使う（次元数が一致する必要あり）。

出力:
    /data/grid_results/v{variant}_a{alpha}.txt  — weights.txt
    /data/grid_results/summary.csv              — variant, alpha, N, D, RMSE の一覧
"""
import argparse
import csv
import pathlib
import sys
import numpy as np
from sklearn.linear_model import Ridge

VARIANT_DIMS = {1: 164, 2: 164 * 21, 3: 164 * 81, 4: 164 * 21 * 81}


def load_xy(x_pattern, y_pattern, variant):
    x_path = x_pattern.replace("{variant}", str(variant))
    y_path = y_pattern.replace("{variant}", str(variant))
    D = VARIANT_DIMS[variant]
    y_raw = np.frombuffer(open(y_path, "rb").read(), dtype=np.float32)
    N = len(y_raw)
    X = np.frombuffer(open(x_path, "rb").read(), dtype=np.float32).reshape(N, D)
    return X, y_raw.astype(np.float64), N, D


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--x",       required=True)
    p.add_argument("--y",       required=True)
    p.add_argument("--variants", type=int, nargs="+", default=[1, 2, 3, 4])
    p.add_argument("--alphas",   type=float, nargs="+", default=[0.1, 0.3, 1.0, 3.0, 10.0])
    p.add_argument("--out_dir",  required=True)
    args = p.parse_args()

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    summary = []
    total = len(args.variants) * len(args.alphas)
    done = 0

    for variant in args.variants:
        try:
            X, y, N, D = load_xy(args.x, args.y, variant)
        except Exception as e:
            print(f"[SKIP] variant={variant}: {e}", file=sys.stderr)
            continue

        for alpha in args.alphas:
            done += 1
            print(f"[{done}/{total}] variant={variant}  alpha={alpha}  N={N}  D={D}")

            model = Ridge(alpha=alpha, fit_intercept=False)
            model.fit(X, y)
            w = model.coef_
            rmse = float(np.sqrt(((y - X @ w) ** 2).mean()))
            print(f"  RMSE={rmse:.2f}  |w|_mean={np.abs(w).mean():.4f}")

            out_path = out_dir / f"v{variant}_a{alpha:.4g}.txt"
            with open(out_path, "w") as f:
                f.write(f"# variant={variant}  alpha={alpha}  N={N}  D={D}  RMSE={rmse:.2f}\n")
                for v in w:
                    f.write(f"{v:.6f}\n")

            summary.append({"variant": variant, "alpha": alpha, "N": N, "D": D,
                            "RMSE": round(rmse, 4), "weights": str(out_path)})

    csv_path = out_dir / "summary.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["variant", "alpha", "N", "D", "RMSE", "weights"])
        writer.writeheader()
        writer.writerows(summary)

    print(f"\n=== summary ===")
    for row in sorted(summary, key=lambda r: r["RMSE"]):
        print(f"  v{row['variant']}  alpha={row['alpha']:<6}  RMSE={row['RMSE']}")
    print(f"\nwrote {csv_path}")


if __name__ == "__main__":
    main()
