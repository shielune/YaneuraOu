#!/usr/bin/env bun
// Node-side WASM eval smoke-test. Loads a built yaneuraou.<pkg>.js through
// node:worker_threads, sends a fixed USI search, and prints the eval as
// the same JSON shape as wasm_eval_browser.ts so wasm_eval_all.sh can
// compare both runners side by side.
//
// Usage:
//   bun script/wasm_eval_node.ts <path-to-yaneuraou.<pkg>.js> [--think-ms 30000]

import { resolve, basename, dirname } from "node:path";
import fs from "node:fs";
import { buildContext, pickLoader } from "./loaders/detect.ts";
import { nodeLoaders } from "./loaders/node/index.ts";
import { runUsiEval } from "./wasm_eval_common.ts";

const SFEN =
  "lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1";
const DEFAULT_THINK_MS = 30000;

const argv = process.argv.slice(2);
if (argv.length < 1) {
  console.error(
    "usage: bun script/wasm_eval_node.ts <yaneuraou.<pkg>.js> [--think-ms N]",
  );
  process.exit(2);
}
const jsPath = resolve(argv[0]!);
let thinkMs = DEFAULT_THINK_MS;
for (let i = 1; i < argv.length; i++) {
  if (argv[i] === "--think-ms") thinkMs = Number(argv[++i]);
}
if (!fs.existsSync(jsPath)) {
  console.error(`not found: ${jsPath}`);
  process.exit(2);
}

const ctx = buildContext("node", jsPath);
// New layout: build/<ver>_<arch>/<pkg>/<variant>/lib/
//   libDir:                                         ^^^
//   dirname(libDir):                    <variant>
//   dirname(dirname(libDir)):           <pkg>
//   dirname(dirname(dirname(libDir))):  <ver>_<arch>
const versionTag = basename(dirname(dirname(dirname(ctx.libDir))));
const engineTag = basename(dirname(dirname(ctx.libDir)));

// The Node loaders install a `console.log` tap to catch emscripten runtime
// stdout, which means we can't use `console.log` to emit our own JSON
// result — it would be swallowed by the tap. Write directly to stdout/stderr
// instead.
const emit = (obj: unknown) => {
  process.stdout.write(JSON.stringify(obj, null, 2) + "\n");
};

async function main() {
  const loader = pickLoader(nodeLoaders, ctx.emscriptenVersion);
  const engine = await loader.load(ctx);
  try {
    const result = await runUsiEval(engine, {
      sfen: SFEN,
      thinkMs,
      threads: 1,
      hash: 64,
    });
    emit({
      runner: "node",
      loader: loader.name,
      version: versionTag,
      engine: engineTag,
      thinkMs,
      ...result,
    });
  } finally {
    await engine.dispose();
  }
}

try {
  await main();
  process.exit(0);
} catch (e) {
  const err = e as Error;
  emit({
    runner: "node",
    version: versionTag,
    engine: engineTag,
    thinkMs,
    error: err.stack ?? String(err),
  });
  process.exit(3);
}
