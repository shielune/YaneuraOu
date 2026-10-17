// Regression test: dispose() must let the engine's shared WebAssembly.Memory go.
//
// OPT-IN. This runs create -> eval -> dispose four times and then reads
// /proc/self/smaps, which takes about half a minute and needs the real eval
// file, so it is not part of the default test run. Enable it with
//
//   YANEURAOU_MEMORY_TEST=1 node test/dispose-memory.mjs     (or: bun ...)
//
// The eval file is read from ../.dl/nn.bin like smoke.mjs; point
// YANEURAOU_EVAL_BIN at another file to override that.
//
// What it guards: the main thread of the module used to park an
// Atomics.waitAsync() on its own pthread mailbox and re-arm it on every wake-up.
// V8 keeps such a pending promise reachable, and it held the emscripten Module
// and its shared memory (about 175 MB for KP256 with USI_Hash 64) for good, so
// every create -> dispose left one more copy behind. source/wasm_pre_node.js
// hides waitAsync from the glue on the main thread. This test needs a dist/
// built with that file; against an older yaneuraou.js it fails, because the
// memory of all four engines is still mapped and resident.
//
// Linux only (it counts the resident size of the shared-memory mappings).

import { fileURLToPath } from "node:url";
import { readFile } from "node:fs/promises";
import fs from "node:fs";
import path from "node:path";
import v8 from "node:v8";
import vm from "node:vm";

import { createEngine } from "../dist/index.js";

if (!process.env.YANEURAOU_MEMORY_TEST) {
  console.log("[dispose-memory] SKIP — opt-in, set YANEURAOU_MEMORY_TEST=1");
  process.exit(0);
}
if (!fs.existsSync("/proc/self/smaps")) {
  console.log("[dispose-memory] SKIP — needs /proc/self/smaps (Linux)");
  process.exit(0);
}

const packageDir = path.dirname(fileURLToPath(import.meta.url)) + "/..";
const enginePath = path.resolve(packageDir, "dist/yaneuraou.js");
const wasmPath = path.resolve(packageDir, "dist/yaneuraou.wasm");
const evalBinPath =
  process.env.YANEURAOU_EVAL_BIN ?? path.resolve(packageDir, "..", ".dl/nn.bin");

const factory = (await import(`file://${enginePath}`)).default;
const wasmBinary = await readFile(wasmPath);
const evalBin = await readFile(evalBinPath);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// A full GC is what lets go of the memory once nothing holds on to it. Bun has
// its own; on Node get `gc` without needing --expose-gc.
let collect;
if (typeof Bun !== "undefined") {
  collect = () => Bun.gc(true);
} else {
  v8.setFlagsFromString("--expose-gc");
  collect = vm.runInNewContext("gc");
}

// Resident size, in MB, of the writable mappings that are 150 MB - 600 MB large.
// The engine's shared memory is one such mapping per module (128 MB initial
// plus the 64 MB hash this test uses, so about 210 MB); the rest of a plain
// Node or Bun process has no mapping that big and resident.
function sharedMemoryRssMb() {
  let total = 0;
  let inside = false;
  for (const line of fs.readFileSync("/proc/self/smaps", "utf8").split("\n")) {
    const m = line.match(/^([0-9a-f]+)-([0-9a-f]+) (\S+) /);
    if (m) {
      const mb = (parseInt(m[2], 16) - parseInt(m[1], 16)) / 1048576;
      inside = m[3].startsWith("rw") && mb >= 150 && mb <= 600;
    } else if (inside && line.startsWith("Rss:")) {
      total += Number(line.split(/\s+/)[1]) / 1024;
    }
  }
  return Math.round(total);
}

const sfen = "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1";
const CYCLES = 4;
// One leaked engine keeps about 175 MB resident. Allow for noise, but not for one.
const MAX_LEFTOVER_MB = 100;

const hadWaitAsync = typeof Atomics.waitAsync;
const failures = [];

for (let cycle = 1; cycle <= CYCLES; cycle++) {
  // The Engine has to be unreachable when the cycle ends, as it is in a real
  // program that plays one game per engine.
  await (async () => {
    const engine = await createEngine({
      factory,
      enginePath,
      wasmBinary,
      evalBin,
      threads: 1,
      usiHash: 64,
    });
    const result = await engine.eval({ sfen, byoyomi: 200 });
    if (!/^[A-Z\d][a-z\d*][1-9][a-z]/.test(result.bestmove)) {
      failures.push(`cycle ${cycle}: bestmove is not a USI move: "${result.bestmove}"`);
    }
    engine.dispose();
  })();

  // Release is asynchronous (workers stop, then a GC frees the memory).
  let rss = sharedMemoryRssMb();
  for (let i = 0; i < 20 && rss > MAX_LEFTOVER_MB; i++) {
    await sleep(500);
    collect();
    rss = sharedMemoryRssMb();
  }
  console.log(`[dispose-memory] cycle ${cycle}: shared memory still resident = ${rss} MB`);
  if (rss > MAX_LEFTOVER_MB) {
    failures.push(
      `cycle ${cycle}: ${rss} MB of engine memory is still resident after dispose() ` +
        `(more than ${MAX_LEFTOVER_MB} MB means a disposed engine was never freed)`,
    );
    break;
  }
}

// The fix must not be a process-wide change.
if (typeof Atomics.waitAsync !== hadWaitAsync) {
  failures.push(
    `Atomics.waitAsync changed from ${hadWaitAsync} to ${typeof Atomics.waitAsync}`,
  );
}

if (failures.length > 0) {
  console.error("\n[dispose-memory] FAIL:");
  for (const f of failures) console.error("  -", f);
  process.exit(1);
}
console.log("\n[dispose-memory] PASS — disposed engines give their memory back.");
process.exit(0);
