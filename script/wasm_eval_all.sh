#!/usr/bin/env bash
# Run the WASM eval smoke-test against every build/<ver>/<pkg>/lib/*.js output
# using the Playwright-based (headless Chromium) runner.
#
# Env:
#   THINK_MS  — thinking time in ms (default 5000)
#   PKG       — restrict to a single package (default: all present)

set -u
cd "$(dirname "$0")/.."

THINK_MS="${THINK_MS:-5000}"
PKG_FILTER="${PKG:-}"
OUT="build/eval_results_$(date +%Y%m%d_%H%M%S).jsonl"

echo "writing results to $OUT"

mapfile -t JS_FILES < <(find build -type f -name 'yaneuraou.*.js' \
  ! -name '*.worker.js' \
  | sort)

for js in "${JS_FILES[@]}"; do
  ver=$(basename "$(dirname "$(dirname "$(dirname "$js")")")")
  pkg=$(basename "$(dirname "$(dirname "$js")")")
  if [[ -n "$PKG_FILTER" && "$pkg" != "$PKG_FILTER" ]]; then continue; fi
  echo "==== [$ver / $pkg] think ${THINK_MS}ms ===="
  result=$(bun script/wasm_eval_browser.ts "$js" --think-ms "$THINK_MS" 2>&1 | tail -40) || true
  echo "$result" | tee -a "$OUT"
  echo "" >> "$OUT"
done
