/**
 * Reproduce the `noInitialRun: true` + direct ccall path for the
 * 3.1.70 node variant. With noInitialRun, main() is NOT called by the
 * factory, so `Options` / `Eval::init()` never run. We then ccall
 * `usi_command('usi')` directly.
 *
 * Expected observation (current build):
 *   - ccall('usi_command', ..., ['usi']) → returns 0 immediately
 *   - Engine emits 'id name YaneuraOu NNUE KP256 ...' / 'id author ...'
 *     / 'usiok' — but NO 'option name ...' block, because Options was
 *     never registered by main().
 *   - setoption fails with 'Error! : No such option: ...'
 *   - isready fails while trying to load an NNUE file that wasn't
 *     embedded yet.
 *
 * This is NOT a workaround for the production flow — it only proves
 * the engine can be driven without main(). Running the real USI flow
 * requires main() to be invoked somehow (see
 * __tests__/node_3.1.70_callmain.ts).
 *
 * Run:
 *   bun __tests__/node_3.1.70_noinit_ccall.ts
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
})) as {
  addMessageListener?: (l: (line: string) => void) => void;
  ccall?: (
    name: string,
    ret: string,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => number;
};

engine.addMessageListener?.((line) => console.error("[aml]", line));

console.error("[test] ccall usi (no prior main)");
const ret = engine.ccall?.("usi_command", "number", ["string"], ["usi"]);
console.error("[test] ccall returned", ret);

await new Promise<void>((r) => setTimeout(r, 2000));
console.error("[test] done");
process.exit(0);
