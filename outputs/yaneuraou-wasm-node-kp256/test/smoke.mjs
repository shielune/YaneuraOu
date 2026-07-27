// Smoke test: drive the package's createEngine() against the staged 3.1.43
// node-variant artefacts in dist/ and verify USI behaviour end-to-end.
//
// What this exercises:
//   1. The package's web-globals shim (self/window/location/document/fetch).
//   2. The Worker polyfill that wraps node:worker_threads.Worker and routes
//      .worker.js through dist/worker_shim.js.
//   3. The noInitialRun + manual callMain bootstrap.
//   4. The USI command/event loop (usi -> usiok, isready -> readyok,
//      position+go -> info+bestmove).

import { fileURLToPath } from "node:url";
import { readFile } from "node:fs/promises";
import path from "node:path";

import { createEngine } from "../dist/index.js";

const packageDir = path.dirname(fileURLToPath(import.meta.url)) + "/..";
const enginePath = path.resolve(packageDir, "dist/yaneuraou.js");
const wasmPath = path.resolve(packageDir, "dist/yaneuraou.wasm");

console.log("[smoke] enginePath:", enginePath);
console.log("[smoke] wasmPath:  ", wasmPath);

// Dynamically import the engine factory the same way a consumer would.
const factoryMod = await import(`file://${enginePath}`);
const factory = factoryMod.default;
if (typeof factory !== "function") {
  throw new Error("engine factory is not a function");
}

const wasmBinary = await readFile(wasmPath);
console.log(`[smoke] wasm size: ${wasmBinary.byteLength} bytes`);

// Print every line the engine emits so we can see why it exits.
process.on("uncaughtException", (e) => {
  console.error("[smoke] uncaught:", e?.name || e, e?.message);
  process.exit(1);
});

// KP256 needs an external eval — suishopetite (~873 KB). Locate it under
// the repo's .dl cache; in real consumer apps users supply this via
// fs.readFile(path/to/nn.bin).
const evalBinPath = path.resolve(packageDir, "..", ".dl/nn.bin");
const evalBin = await readFile(evalBinPath);
console.log(`[smoke] eval bin: ${evalBinPath} (${evalBin.byteLength} bytes)`);

console.log("[smoke] creating engine (threads=1, usiHash=16)...");
const t0 = Date.now();
const engine = await createEngine({
  factory,
  enginePath,
  wasmBinary,
  evalBin,
  threads: 1,
  usiHash: 16,
  handshakeTimeoutMs: 30_000,
  readyTimeoutMs: 60_000,
});
console.log(`[smoke] engine ready in ${Date.now() - t0}ms`);

const sfen =
  "lr5nl/2P2+S1k1/3p3pp/p2bppp2/1p1s1B3/P1P3P2/3PPP1PP/2K6/LN1G3RL b BG2S5Pgsnp 1";

console.log("[smoke] eval at depth=24, byoyomi=10000ms...");
const evalStart = Date.now();
const result = await engine.eval({
  sfen,
  byoyomi: 10_000,
  depthLimit: 24,
});
const evalMs = Date.now() - evalStart;

console.log("[smoke] result:");
console.log("  bestmove :", result.bestmove);
console.log("  ponder   :", result.ponder);
console.log("  score    :", result.score);
console.log("  depth    :", result.depth);
console.log("  nodes    :", result.nodes);
console.log("  timeMs   :", result.timeMs);
console.log("  wall ms  :", evalMs);
console.log("  lastInfo :", result.lastInfo);

engine.dispose();

// Sanity checks. The docs/wasm_upgrade_changelog.md "depth 24 / cp 381 / G*9g"
// figure is the HalfKP256 (Suisho5) baseline, not KP256 — different eval
// architecture → different score and bestmove. So we don't pin exact values
// here; instead we check that the engine produced a structurally sane
// result with KP256 eval loaded successfully.
const failures = [];
if (!/^[A-Z\d][a-z\d*][1-9][a-z]/.test(result.bestmove)) {
  failures.push(`bestmove not a valid USI move: "${result.bestmove}"`);
}
if (!result.score || result.score.kind !== "cp") {
  failures.push(
    `score: expected a cp score, got ${JSON.stringify(result.score)}`,
  );
}
if (result.depth === null || result.depth < 1) {
  failures.push(`depth: expected >= 1, got ${result.depth}`);
}
if (result.nodes === null || result.nodes < 1000) {
  failures.push(`nodes: expected > 1000, got ${result.nodes}`);
}

if (failures.length > 0) {
  console.error("\n[smoke] FAIL:");
  for (const f of failures) console.error("  -", f);
  process.exit(1);
}

console.log(
  "\n[smoke] PASS — engine bootstrapped, eval loaded, USI driver completed " +
    "search and returned a structurally-valid result.",
);
