/**
 * Test `noInitialRun: true` + explicit `engine.callMain([])` on the
 * 3.1.70 node variant.
 *
 * Theory: main() in source/main.cpp runs Options / NNUE init and then
 * returns under __EMSCRIPTEN__ (the USI::loop block is #if'd out), so
 * explicitly calling it manually should give us fully initialised
 * engine state without the mysterious deadlock from the
 * auto-initialised path. callMain is exposed via
 * `EXPORTED_RUNTIME_METHODS=['FS','ccall','callMain']` in the node
 * variant (see source/Makefile and script/wasm_build.js's `variants`).
 *
 * Observation (current build):
 *   - engine.callMain([]) returns 0 without producing any stdout or
 *     triggering addMessageListener.
 *   - Subsequent engine.ccall('usi_command', ..., ['usi']) returns 1
 *     ("busy") indefinitely.
 *   - engine.postMessage('usi') / 'isready' never drain.
 *
 * Hypothesis: pthread pool start-up is racing with main(), or the
 * emscripten Node pthread handshake is broken in a way that leaves
 * `usi_command` locked waiting for a worker that never comes up.
 * See docs/wasm_eval_results.md for the ongoing investigation.
 *
 * Run:
 *   bun __tests__/node_3.1.70_callmain.ts
 */

import fs from "node:fs";
import path from "node:path";
import url from "node:url";

const jsPath = path.resolve("build/3.1.70_x86_64/k-p/node/lib/yaneuraou.k-p.js");
if (!fs.existsSync(jsPath)) {
  console.error(`[test] not found: ${jsPath}`);
  process.exit(2);
}

const libDir = path.dirname(jsPath);
if (!fs.existsSync(path.join(libDir, "package.json"))) {
  fs.writeFileSync(path.join(libDir, "package.json"), '{"type":"module"}\n');
}

const mod = await import(url.pathToFileURL(jsPath).href);
const factory = (mod as { default?: (opts?: unknown) => Promise<unknown> })
  .default ?? (mod as unknown as (opts?: unknown) => Promise<unknown>);

console.error("[test] factory with noInitialRun=true");
const engine = (await factory({
  noInitialRun: true,
  print: (s: string) => console.error("[print]", s),
  printErr: (s: string) => console.error("[printErr]", s),
  postRun: () => console.error("[postRun fired]"),
})) as {
  addMessageListener?: (l: (line: string) => void) => void;
  postMessage?: (cmd: string) => void;
  ccall?: (
    name: string,
    ret: string,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => number;
  callMain?: (args: readonly string[]) => number;
};

engine.addMessageListener?.((line) => console.error("[aml]", line));

console.error("[test] calling callMain explicitly");
try {
  const ret = engine.callMain?.([]);
  console.error("[test] callMain returned", ret);
} catch (e) {
  const err = e as Error;
  console.error("[test] callMain threw", err.name, err.message);
}

await new Promise<void>((r) => setTimeout(r, 1000));

console.error("[test] ccall usi");
const r = engine.ccall?.("usi_command", "number", ["string"], ["usi"]);
console.error("[test] ccall returned", r);

await new Promise<void>((r) => setTimeout(r, 2000));
console.error("[test] done");
process.exit(0);
