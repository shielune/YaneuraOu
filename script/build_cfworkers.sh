#!/usr/bin/env bash
# Build YaneuraOu k-p.noeval for Cloudflare Workers (edge variant).
#
# Usage:
#   script/build_cfworkers.sh [EMSDK_VERSION]
#
# Default EMSDK_VERSION: 5.0.5
# The build runs inside docker (emscripten/emsdk) and produces:
#   yaneuraou-cfworkers/dist/yaneuraou.js
#   yaneuraou-cfworkers/dist/yaneuraou.wasm
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EMSDK_VERSION="${1:-5.0.5}"
DIST_DIR="$ROOT_DIR/yaneuraou-cfworkers/dist"

# Workers memory constraints:
#   Paid plan: 128 MB per isolate (WASM + JS heap combined).
#   64 MB initial + growth up to 128 MB keeps room for the JS heap.
#   Stack 2 MB is sufficient for single-threaded search.
EM_INITIAL_MEMORY_SIZE=67108864      # 64 MB
EM_MAXIMUM_MEMORY_SIZE=134217728     # 128 MB
EM_STACK_SIZE=2097152                # 2 MB

EDITION="YANEURAOU_ENGINE_NNUE_KP256"
EXPORT_NAME="YaneuraOu_K_P"
CPUS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

echo "[cfworkers] emsdk=${EMSDK_VERSION}  edition=${EDITION}"
echo "[cfworkers] memory: initial=${EM_INITIAL_MEMORY_SIZE} max=${EM_MAXIMUM_MEMORY_SIZE} stack=${EM_STACK_SIZE}"

# Prepare output directory
mkdir -p "$DIST_DIR"

# Build inside Docker
docker pull "emscripten/emsdk:${EMSDK_VERSION}"

docker run --rm \
  -v "$ROOT_DIR:/src" \
  -w /src/source \
  "emscripten/emsdk:${EMSDK_VERSION}" \
  make "-j${CPUS}" clean tournament \
    COMPILER=em++ \
    TARGET_CPU=WASM \
    YANEURAOU_EDITION="${EDITION}" \
    TARGET="../yaneuraou-cfworkers/dist/yaneuraou.js" \
    EM_EXPORT_NAME="${EXPORT_NAME}" \
    EM_ENVIRONMENT=web \
    "EM_EXPORTED_RUNTIME_METHODS=['FS','ccall']" \
    EM_PTHREAD=0 \
    EM_INITIAL_MEMORY_SIZE="${EM_INITIAL_MEMORY_SIZE}" \
    EM_MAXIMUM_MEMORY_SIZE="${EM_MAXIMUM_MEMORY_SIZE}" \
    EM_STACK_SIZE="${EM_STACK_SIZE}"

# Verify artifacts
for f in yaneuraou.js yaneuraou.wasm; do
  if [ ! -f "$DIST_DIR/$f" ]; then
    echo "[cfworkers] ERROR: $DIST_DIR/$f not found" >&2
    exit 1
  fi
done

# Compress
for f in "$DIST_DIR/yaneuraou.js" "$DIST_DIR/yaneuraou.wasm"; do
  brotli -f --best "$f" -o "${f}.br" 2>/dev/null || true
  gzip  -f --best -k "$f" 2>/dev/null || true
done

# Size report
echo ""
echo "[cfworkers] build complete:"
ls -lh "$DIST_DIR"/yaneuraou.{js,wasm}
echo ""
echo "[cfworkers] compressed:"
ls -lh "$DIST_DIR"/yaneuraou.{js,wasm}.{br,gz} 2>/dev/null || true
