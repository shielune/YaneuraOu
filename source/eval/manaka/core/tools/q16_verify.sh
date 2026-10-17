#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 1 ]]; then
  echo 'usage: q16_verify.sh quant-reference-directory' >&2
  exit 2
fi
quant="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
# CORE_ROOT, OUTPUT_ROOT, CXX and SANITIZE are passed through to q16_build.sh.
core="${CORE_ROOT:-$root}"
out="${OUTPUT_ROOT:-$core/build-q16}"
bash "$root/tools/q16_build.sh"
suffix="${SANITIZE:+_asan}"
if [[ ${SANITIZE:-0} != 1 ]]; then suffix=; fi
for model in pair16_d128_o3_h512 pair16_d128_o3_h512_h2256; do
  "$out/q16_driver$suffix" "$quant/ref-$model.nn.bin" \
    "$quant/ref-$model-ply33.tsv" "$quant/ref-$model-mixed.tsv" "$quant/ref-$model-dyn.tsv"
done
"$out/q16_corrupt$suffix" "$quant/ref-pair16_d128_o3_h512.nn.bin" \
  "$quant/ref-pair16_d128_o3_h512_h2256.nn.bin" "$out/corrupt.bin"
