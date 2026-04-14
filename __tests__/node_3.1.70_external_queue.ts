/**
 * Drive the 3.1.70 node variant engine through
 * `script/loaders/external_queue.ts`, which replaces `engine.postMessage`
 * with an out-of-band pump that calls `engine.ccall("usi_command", ...)`
 * directly. This bypasses `wasm_pre.js`'s closure-scoped queue, which
 * only drains from `Module.postRun` — a callback that never fires on
 * this generation under Node.
 *
 * Observation (current build):
 *   - The external queue pump keeps ccall returning 1 ("busy") on
 *     every attempt, just like the direct-ccall path. This confirms
 *     the stall is not caused by wasm_pre.js's queue machinery but by
 *     the engine itself being stuck waiting for something at the
 *     pthread layer.
 *
 * Run:
 *   bun __tests__/node_3.1.70_external_queue.ts
 */

import fs from "node:fs";
import path from "node:path";
import url from "node:url";
import { installExternalQueue } from "../script/loaders/external_queue.ts";

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

const engine = (await factory({
  print: (s: string) => console.error("[print]", s),
  printErr: (s: string) => console.error("[printErr]", s),
})) as {
  ccall?: (
    name: string,
    ret: string,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => number;
  postMessage?: (cmd: string) => void;
  addMessageListener?: (l: (line: string) => void) => void;
};

engine.addMessageListener?.((line) => console.error("[aml]", line));

const enqueue = installExternalQueue(
  engine as Parameters<typeof installExternalQueue>[0],
);

console.error("[test] enqueue usi via external pump");
enqueue("usi");

await new Promise<void>((r) => setTimeout(r, 3000));
console.error("[test] done");
process.exit(0);
