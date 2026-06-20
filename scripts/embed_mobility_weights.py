#!/usr/bin/env python3
"""
weights.txt → mobility_weights_embedded.cpp (バリアント1のみ) または eval.bin (バリアント2〜4)。

Usage:
    # バリアント1: cpp に焼き込む
    python scripts/embed_mobility_weights.py \
        --weights /data/weights.txt \
        --variant 1 \
        --source  source/eval/mobility/mobility_weights_embedded.cpp

    # バリアント2〜4: eval.bin として出力 (EvalDir に置く)
    python scripts/embed_mobility_weights.py \
        --weights /data/weights.txt \
        --variant 2 \
        --out /path/to/eval/eval.bin
"""
import argparse
import pathlib
import struct
import textwrap

VARIANT_DIMS = {1: 164, 2: 164 * 21, 3: 164 * 81, 4: 164 * 21 * 81}


def load_weights(path):
    vals = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            vals.append(float(line))
    return vals


def embed_v1(vals, source_path, label):
    entries = ",\n    ".join(f"{v:.6f}f" for v in vals)
    cpp = textwrap.dedent(f"""\
        // AUTO-GENERATED. Do not edit by hand.
        // Source : {label}
        // Tool   : scripts/embed_mobility_weights.py

        #include "../../config.h"

        #if defined(EVAL_MOBILITY)

        namespace Eval {{ namespace Mobility {{

        extern const int kEmbeddedMobilityWeightsSize = {len(vals)};

        extern const float kEmbeddedMobilityWeights[{len(vals)}] = {{
            {entries}
        }};

        }} }} // namespace Eval::Mobility

        #endif // EVAL_MOBILITY
        """)
    out = pathlib.Path(source_path)
    out.write_text(cpp)
    print(f"wrote {len(vals)} weights → {out}")


def embed_bin(vals, out_path):
    data = struct.pack(f"{len(vals)}f", *vals)
    pathlib.Path(out_path).write_bytes(data)
    print(f"wrote {len(vals)} weights → {out_path}  ({len(data)} bytes)")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--weights", required=True)
    p.add_argument("--variant", type=int, default=1, choices=[1, 2, 3, 4])
    p.add_argument("--source",  default=None,
                   help="バリアント1: 書き込み先 mobility_weights_embedded.cpp")
    p.add_argument("--out",     default=None,
                   help="バリアント2〜4: 出力 eval.bin のパス")
    args = p.parse_args()

    expected = VARIANT_DIMS[args.variant]
    vals = load_weights(args.weights)
    if len(vals) != expected:
        raise ValueError(f"variant {args.variant} expects {expected} weights, got {len(vals)}")

    label = pathlib.Path(args.weights).name

    if args.variant == 1:
        if not args.source:
            raise ValueError("--source が必要です (バリアント1)")
        embed_v1(vals, args.source, label)
    else:
        if not args.out:
            raise ValueError("--out が必要です (バリアント2〜4)")
        embed_bin(vals, args.out)


if __name__ == "__main__":
    main()
