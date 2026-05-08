#!/usr/bin/env bash
# Build YaneuraOu k-p.noeval for Cloudflare Workers via act.
#
# Usage:
#   script/build_cfworkers.sh [EMSDK_VERSION]
#
# Default EMSDK_VERSION: 5.0.5
# Runs .github/workflows/build-cfworkers.yml locally with act.
# Artifacts are produced in yaneuraou-cfworkers/dist/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EMSDK_VERSION="${1:-5.0.5}"

echo "[cfworkers] running act with emsdk=${EMSDK_VERSION}"

cd "$ROOT_DIR"

act workflow_dispatch \
  -W .github/workflows/build-cfworkers.yml \
  --input "emsdk_version=${EMSDK_VERSION}" \
  --bind
