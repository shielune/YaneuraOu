#!/usr/bin/env bash
# Usage: bash tools/check_reference.sh WEIGHTS_DIR REFERENCE_DIR [BUILD_DIR]
# REFERENCE_DIR contains positions-{ply33,mixed}-4000.sfen and values-*.tsv.
# Run from core/ after make. Outputs remain under BUILD_DIR/results/.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 WEIGHTS_DIR REFERENCE_DIR [BUILD_DIR]" >&2
  exit 2
fi
weights=$1
reference=$2
build=${3:-build}
root=$(cd "$(dirname "$0")/.." && pwd)
if [[ $build != /* ]]; then build="$root/$build"; fi
mkdir -p "$build/results"
for shape in pair16_d128_o3_h512 pair16_d128_o3_h512_h2256; do
  for subset in ply33 mixed; do
    name="$shape-$subset"
    "$build/eval_dump" \
      --weights "$weights/${shape}_rescored.mnw" \
      --sfens "$reference/positions-$subset-4000.sfen" --limit 4000 \
      > "$build/results/values-$name.tsv" 2> "$build/results/values-$name.err"
    python3 -I "$root/tools/compare_tsv.py" \
      "$reference/values-$name.tsv" "$build/results/values-$name.tsv" \
      | tee "$build/results/compare-$name.txt"
    cmp "$reference/values-$name.tsv" "$build/results/values-$name.tsv"
    echo "$name: TSV text-identical"
  done
done
