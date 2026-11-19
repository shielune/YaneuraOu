#!/usr/bin/env bash
set -euo pipefail

OBJ=/home/vscode/app/obj
SRC=/home/vscode/app/source
NPROC=$(nproc)

mkdir -p /home/vscode/app/engine/arm64 /home/vscode/app/engine/amd64

echo "=== [1/4] arm64 Mobility ==="
find "$OBJ" -name "*.o" -delete 2>/dev/null || true
make -C "$SRC" -j"$NPROC" normal \
  YANEURAOU_EDITION=YANEURAOU_ENGINE_KIKI \
  TARGET_CPU=GRAVITON2 \
  COMPILER=g++ \
  USE_HUMANLIKE_OPTIONS=OFF \
  TARGET=../engine/arm64/YaneuraOu-Mobility-NoEval

echo "=== [2/4] arm64 Material ==="
find "$OBJ" -name "*.o" -delete 2>/dev/null || true
make -C "$SRC" -j"$NPROC" normal \
  YANEURAOU_EDITION=YANEURAOU_ENGINE_MATERIAL \
  TARGET_CPU=GRAVITON2 \
  COMPILER=g++ \
  USE_HUMANLIKE_OPTIONS=OFF \
  TARGET=../engine/arm64/YaneuraOu-Material-NoEval

echo "=== [3/4] amd64 Mobility ==="
find "$OBJ" -name "*.o" -delete 2>/dev/null || true
make -C "$SRC" -j"$NPROC" normal \
  YANEURAOU_EDITION=YANEURAOU_ENGINE_KIKI \
  TARGET_CPU=AVX2 \
  COMPILER=x86_64-linux-gnu-g++ \
  USE_HUMANLIKE_OPTIONS=OFF \
  TARGET=../engine/amd64/YaneuraOu-Mobility-NoEval

echo "=== [4/4] amd64 Material ==="
find "$OBJ" -name "*.o" -delete 2>/dev/null || true
make -C "$SRC" -j"$NPROC" normal \
  YANEURAOU_EDITION=YANEURAOU_ENGINE_MATERIAL \
  TARGET_CPU=AVX2 \
  COMPILER=x86_64-linux-gnu-g++ \
  USE_HUMANLIKE_OPTIONS=OFF \
  TARGET=../engine/amd64/YaneuraOu-Material-NoEval

echo "=== Done ==="
