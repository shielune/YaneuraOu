// Regression test: a createEngine() that throws must not leave the engine's
// pthread workers behind.
//
// createEngine() hands the caller an Engine (the only thing that can
// dispose() the module) only once the engine is ready. When it throws on the
// way — here because the eval file is garbage and the engine calls exit(1)
// during `isready`; a readyok timeout takes the same path — there is nothing
// to dispose, and every one of the 32 pooled pthread workers used to stay
// alive together with the memory they hold.
//
// This test uses a 1 KB eval file of zeros, so it does not need the real
// nn.bin (smoke.mjs does).
//
// Usage (after `bun run build:loader` and staging yaneuraou.{js,wasm,worker.js}
// into dist/, same as smoke.mjs):
//
//   node test/create-engine-failure.mjs      (or: bun test/create-engine-failure.mjs)
//
// The check counts the threads of the test process, so it runs on Linux only
// (it prints SKIP elsewhere).

import { fileURLToPath } from "node:url";
import { readFile } from "node:fs/promises";
import fs from "node:fs";
import path from "node:path";

import { createEngine } from "../dist/index.js";

const packageDir = path.dirname(fileURLToPath(import.meta.url)) + "/..";
const enginePath = path.resolve(packageDir, "dist/yaneuraou.js");
const wasmPath = path.resolve(packageDir, "dist/yaneuraou.wasm");

if (!fs.existsSync("/proc/self/task")) {
  console.log("[create-engine-failure] SKIP — needs /proc/self/task (Linux)");
  process.exit(0);
}
const threadCount = () => fs.readdirSync("/proc/self/task").length;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const factory = (await import(`file://${enginePath}`)).default;
const wasmBinary = await readFile(wasmPath);

// The engine is built with PTHREAD_POOL_SIZE=32. A leaked module keeps all of
// them for good; a cleaned-up one gives them back. Bun hands the threads of a
// terminated worker back a few seconds late and keeps a few helper threads of
// its own, so wait up to 20 s and only require that more than half of the pool
// is gone.
const POOL = 32;
const MAX_LEFTOVER = POOL / 2;

const failures = [];
for (let cycle = 1; cycle <= 2; cycle++) {
  const before = threadCount();

  let error;
  try {
    const engine = await createEngine({
      factory,
      enginePath,
      wasmBinary,
      evalBin: new Uint8Array(1024),
      threads: 1,
      usiHash: 16,
      readyTimeoutMs: 3_000,
    });
    engine.dispose();
  } catch (e) {
    error = e;
  }
  if (!error) {
    failures.push(`cycle ${cycle}: createEngine() accepted a broken eval file`);
    break;
  }
  const peak = threadCount();

  // Worker termination is asynchronous.
  let after = peak;
  for (let i = 0; i < 200; i++) {
    after = threadCount();
    if (after - before < MAX_LEFTOVER) break;
    await sleep(100);
  }

  console.log(
    `[create-engine-failure] cycle ${cycle}: "${error.message}" — ` +
      `threads before=${before} at throw=${peak} settled=${after}`,
  );
  if (after - before >= MAX_LEFTOVER) {
    failures.push(
      `cycle ${cycle}: ${after - before} threads still alive after ` +
        `createEngine() threw (pool of ${POOL} was not torn down)`,
    );
    break;
  }
}

if (failures.length > 0) {
  console.error("\n[create-engine-failure] FAIL:");
  for (const f of failures) console.error("  -", f);
  process.exit(1);
}
console.log(
  "\n[create-engine-failure] PASS — a failing createEngine() cleans up its workers.",
);
process.exit(0);
