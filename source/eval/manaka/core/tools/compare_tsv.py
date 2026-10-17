#!/usr/bin/env python3
"""eval_dump と Rust の dump_eval が出した TSV を、1 局面ずつ突き合わせる。

    python3 -I compare_tsv.py REFERENCE.tsv CANDIDATE.tsv [--top N] [--threshold X]

列は index, sfen, value_stm, pre_stm, value_black, pre_black。行は index と sfen が一致している必要がある。
value_stm と pre_stm(と、先手から見た 2 列)について、差の絶対値の最大、平均、分位数、f32 のビットが完全に
一致した局面の数、しきい値を超えた局面の数を出す。差の大きい局面は index と sfen を添えて並べる。
差は f32 の値を倍精度で引いたもの。pre の差が主で、value は tanh で飽和すると差が縮む。
標準ライブラリだけで動く(python3 -I で走る)。
"""

import argparse
import math
import struct
import sys

COLUMNS = ("value_stm", "pre_stm", "value_black", "pre_black")


def f32_bits(text):
    return struct.unpack("<I", struct.pack("<f", float(text)))[0]


def f32_value(text):
    return struct.unpack("<f", struct.pack("<f", float(text)))[0]


def read_tsv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 6:
                sys.exit(f"{path}:{number}: 6 列ではない({len(parts)} 列)")
            rows.append(parts)
    return rows


def percentile(sorted_values, q):
    if not sorted_values:
        return 0.0
    pos = q * (len(sorted_values) - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (pos - lo)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--top", type=int, default=5, help="差の大きい局面を何個並べるか")
    ap.add_argument("--threshold", type=float, action="append", default=None, help="この値を超えた局面を数える(複数可、既定 1e-6 と 1e-5)")
    args = ap.parse_args()
    thresholds = args.threshold or [1e-6, 1e-5]

    ref = read_tsv(args.reference)
    cand = read_tsv(args.candidate)
    if not ref:
        sys.exit("参照 TSV に局面が無い")
    if len(ref) != len(cand):
        sys.exit(f"行数が違う: {args.reference} は {len(ref)} 行、{args.candidate} は {len(cand)} 行")
    for i, (a, b) in enumerate(zip(ref, cand)):
        if a[0] != b[0] or a[1] != b[1]:
            sys.exit(f"{i + 1} 行目で index か sfen が違う:\n  {a[0]} {a[1]}\n  {b[0]} {b[1]}")

    print(f"positions {len(ref)}")
    for col_index, name in enumerate(COLUMNS, start=2):
        diffs = []
        identical = 0
        nonfinite = 0
        for a, b in zip(ref, cand):
            x, y = f32_value(a[col_index]), f32_value(b[col_index])
            if not (math.isfinite(x) and math.isfinite(y)):
                nonfinite += 1
                diffs.append(math.inf if x != y else 0.0)
                continue
            if f32_bits(a[col_index]) == f32_bits(b[col_index]):
                identical += 1
            diffs.append(abs(x - y))
        ordered = sorted(diffs)
        mean = sum(diffs) / len(diffs) if diffs else 0.0
        print(f"{name}:")
        print(f"  max {ordered[-1]:.3e}  mean {mean:.3e}  p50 {percentile(ordered, 0.5):.3e}  p90 {percentile(ordered, 0.9):.3e}"
              f"  p99 {percentile(ordered, 0.99):.3e}  p99.9 {percentile(ordered, 0.999):.3e}")
        over = "  ".join(f"> {t:g}: {sum(1 for d in diffs if d > t)}" for t in thresholds)
        print(f"  bit-identical {identical}/{len(diffs)}  {over}  non-finite {nonfinite}")
        if name in ("value_stm", "pre_stm") and args.top > 0 and ordered[-1] > 0:
            worst = sorted(range(len(diffs)), key=lambda i: diffs[i], reverse=True)[: args.top]
            for i in worst:
                if diffs[i] == 0:
                    break
                print(f"    #{ref[i][0]} diff {diffs[i]:.3e} ref {ref[i][col_index]} cand {cand[i][col_index]}  {ref[i][1]}")


if __name__ == "__main__":
    main()
