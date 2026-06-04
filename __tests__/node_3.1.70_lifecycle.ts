/**
 * Lifecycle trace for the 3.1.70 node variant. Records the order /
 * timing of preRun / onRuntimeInitialized / postRun / print /
 * addMessageListener so we can see which callbacks the emscripten Node
 * runtime actually invokes on this generation.
 *
 * Expected observation (current build):
 *   - [preRun] fires
 *   - [onRuntimeInitialized] fires
 *   - [postRun] DOES NOT fire → wasm_pre.js's drain never starts
 *   - engine.postMessage('usi') → no response
 *   - engine.ccall('usi_command', ..., ['usi']) → returns 1 (busy)
 *
 * Run:
 *   bun __tests__/node_3.1.70_lifecycle.ts
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

console.error("[test] calling factory (no noInitialRun)");
const t0 = Date.now();
const engine = (await factory({
  print: (s: string) =>
    console.error(`[t=${Date.now() - t0}] [print] ${s}`),
  printErr: (s: string) =>
    console.error(`[t=${Date.now() - t0}] [printErr] ${s}`),
  preRun: () => console.error(`[t=${Date.now() - t0}] [preRun]`),
  onRuntimeInitialized: () =>
    console.error(`[t=${Date.now() - t0}] [onRuntimeInitialized]`),
  postRun: () => console.error(`[t=${Date.now() - t0}] [postRun]`),
  onAbort: (r: unknown) =>
    console.error(`[t=${Date.now() - t0}] [onAbort] ${String(r)}`),
  onExit: (s: unknown) =>
    console.error(`[t=${Date.now() - t0}] [onExit] ${String(s)}`),
})) as {
  addMessageListener?: (l: (line: string) => void) => void;
  postMessage?: (cmd: string) => void;
  ccall?: (
    name: string,
    ret: string,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => number;
};

console.error(`[t=${Date.now() - t0}] factory returned`);

engine.addMessageListener?.((line) => console.error("[aml]", line));

await new Promise<void>((r) => setTimeout(r, 2000));
console.error(`[t=${Date.now() - t0}] 2s after factory`);

console.error("[test] sending usi via postMessage");
engine.postMessage?.("usi");
await new Promise<void>((r) => setTimeout(r, 2000));

console.error("[test] sending usi via ccall");
const ret = engine.ccall?.("usi_command", "number", ["string"], ["usi"]);
console.error("[test] ccall returned", ret);

await new Promise<void>((r) => setTimeout(r, 2000));
console.error("[test] done");
process.exit(0);
