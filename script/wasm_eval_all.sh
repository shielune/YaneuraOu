#!/usr/bin/env bash
# Run the WASM eval smoke-test against every build variant.
#
# Layout produced by script/wasm_build.js:
#   build/<ver>_<arch>/<pkg>/web/lib/yaneuraou.<pkg>.js   → browser runner
#   build/<ver>_<arch>/<pkg>/node/lib/yaneuraou.<pkg>.js  → node runner
#
# Each runner is fed only the variant that matches its environment.
#
# Env:
#   THINK_MS   — thinking time in ms (default 5000)
#   PKG        — restrict to a single package (default: all present)
#   RUNNERS    — comma-separated subset of {node,browser} (default: both)

set -u
cd "$(dirname "$0")/.."

THINK_MS="${THINK_MS:-5000}"
PKG_FILTER="${PKG:-}"
RUNNERS="${RUNNERS:-node,browser}"
OUT="build/eval_results_$(date +%Y%m%d_%H%M%S).jsonl"

echo "writing results to $OUT (runners=$RUNNERS, think=${THINK_MS}ms)"

collect_for_variant() {
  local variant="$1"
  find build -type f -path "*/${variant}/lib/yaneuraou.*.js" \
    ! -name '*.worker.js' \
    | sort
}

run_one() {
  local runner="$1"
  local js="$2"
  local script
  case "$runner" in
    node)    script="script/wasm_eval_node.ts" ;;
    browser) script="script/wasm_eval_browser.ts" ;;
    *)       echo "unknown runner: $runner" >&2; return 2 ;;
  esac
  bun "$script" "$js" --think-ms "$THINK_MS" 2>&1 | tail -60
}

IFS=',' read -ra RUNNER_LIST <<<"$RUNNERS"

for runner in "${RUNNER_LIST[@]}"; do
  case "$runner" in
    node)    variant="node" ;;
    browser) variant="web" ;;
    *)       echo "unknown runner: $runner" >&2; continue ;;
  esac

  mapfile -t JS_FILES < <(collect_for_variant "$variant")

  for js in "${JS_FILES[@]}"; do
    ver=$(basename "$(dirname "$(dirname "$(dirname "$(dirname "$js")")")")")
    pkg=$(basename "$(dirname "$(dirname "$(dirname "$js")")")")
    if [[ -n "$PKG_FILTER" && "$pkg" != "$PKG_FILTER" ]]; then continue; fi
    echo "==== [$ver / $pkg / $runner] think ${THINK_MS}ms ===="
    result=$(run_one "$runner" "$js") || true
    echo "$result" | tee -a "$OUT"
    echo "" >> "$OUT"
  done
done

echo "==== wasm_eval_all done ===="
