#!/usr/bin/env bash
set -euo pipefail
# Usage: [CORE_ROOT=directory] [OUTPUT_ROOT=directory] [CXX=compiler] [SANITIZE=1] q16_build.sh
root="$(cd "$(dirname "$0")/.." && pwd)"
core="${CORE_ROOT:-$root}"
out="${OUTPUT_ROOT:-$core/build-q16}"
cxx="${CXX:-g++}"
mkdir -p "$out"
flags=(-std=c++17 -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math -mavx2)
if [[ ${SANITIZE:-0} == 1 ]]; then
  flags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  target=q16_driver_asan
else
  flags+=(-O3)
  target=q16_driver
fi
for tool in driver corrupt tomc; do
  "$cxx" "${flags[@]}" -I"$core" "$root/tools/q16_$tool.cpp" "$core/q16.cpp" "$core/tokens.cpp" -o "$out/${target/driver/$tool}"
done
