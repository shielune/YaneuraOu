#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."

HOST_SRC="${HOST_SRC:-/Users/devonly/Developer/personal/YaneuraOu}"
PKG="${PKG:-k-p}"
LOGDIR="build/multibuild_logs"
mkdir -p "$LOGDIR"

# Multi-version smoke list.
#
# 5.0.0 is the canonical pinned target for the YaneuraOu upgrade
# (Makefile's `make build` defaults to it). The other versions are
# kept here so future regressions can be re-bisected against the
# same matrix the dual-runner smoke documented in
# docs/wasm_eval_results.md was generated from.
VERSIONS=(
  3.1.50
  3.1.60
  3.1.70
  3.1.74
  4.0.0
  4.0.11
  4.0.23
  5.0.0
  5.0.5
)

for v in "${VERSIONS[@]}"; do
  log="$LOGDIR/${v}_${PKG}.log"
  echo "==== [$(date -Iseconds)] building $v ($PKG) ===="
  if docker run --rm \
      -v "$HOST_SRC":/src -w /src \
      "emscripten/emsdk:$v" \
      node script/wasm_build.js "$PKG" \
      >"$log" 2>&1; then
    echo "  -> OK ($v): $(find build -maxdepth 4 -name 'yaneuraou.'"$PKG"'.wasm' -path "*${v}*" -printf '%p (%s bytes)\n' 2>/dev/null)"
  else
    echo "  -> FAIL ($v) — see $log"
    tail -5 "$log" | sed 's/^/     /'
  fi
done

echo "==== multibuild done ===="
