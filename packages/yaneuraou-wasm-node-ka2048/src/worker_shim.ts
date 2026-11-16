// worker_threads-side shim for emscripten 3.1.43's classic pthread worker.
//
// emscripten emits `yaneuraou.worker.js` as a *classic* script with
// top-level `require("worker_threads")` etc. When the loader spawns
// `node:worker_threads.Worker(shimUrl, {workerData:{workerFile}})`, Node
// runs this shim as an ES module first. The shim:
//
//   1. installs the web-worker-shaped globals emscripten's pthread runtime
//      expects (`self`, `name="em-pthread"`, `importScripts`, `location`,
//      `postMessage`, `onmessage`),
//   2. forwards parentPort <-> globalThis messages so the main thread's
//      Worker polyfill can talk to it the same way a web Worker would,
//   3. shims `fetch(file://)` since Node's undici rejects it but emscripten
//      uses it to load sibling resources,
//   4. eval's the classic `yaneuraou.worker.js` into the global scope,
//      effectively turning it into a faked-web-worker entry point.
//
// Same shape as `script/loaders/node/worker_shim.ts` in the repo, minus the
// teeStdout / dynamic-import branches we don't need for 3.1.43.

import { parentPort, workerData } from "node:worker_threads";
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath, pathToFileURL } from "node:url";

const data = workerData as { workerFile: string };
const g = globalThis as Record<string, unknown>;

g.self = globalThis;
try {
  Object.defineProperty(globalThis, "name", {
    value: "em-pthread",
    writable: true,
    configurable: true,
  });
} catch {
  g.name = "em-pthread";
}
g.importScripts = () => {};
g.location = { href: pathToFileURL(data.workerFile).href };

g.postMessage = (msg: unknown, transfer?: readonly unknown[]) =>
  parentPort!.postMessage(msg, transfer as never);

let onmessage: ((e: { data: unknown }) => void) | null = null;
const pending: unknown[] = [];
Object.defineProperty(globalThis, "onmessage", {
  configurable: true,
  get() {
    return onmessage;
  },
  set(fn: ((e: { data: unknown }) => void) | null) {
    onmessage = fn;
    if (fn && pending.length) {
      for (const m of pending.splice(0)) fn({ data: m });
    }
  },
});
Object.defineProperty(globalThis, "onunhandledrejection", {
  configurable: true,
  writable: true,
  value: null,
});

parentPort!.on("message", (msg: unknown) => {
  if (onmessage) onmessage({ data: msg });
  else pending.push(msg);
});

const origFetch = g.fetch as
  | ((input: unknown, init?: unknown) => Promise<Response>)
  | undefined;
g.fetch = (async (input: unknown, init?: unknown): Promise<Response> => {
  const s =
    typeof input === "string"
      ? input
      : (input as { url?: string })?.url ?? String(input);
  if (s.startsWith("file://")) {
    const buf = fs.readFileSync(fileURLToPath(s));
    return new Response(buf);
  }
  return origFetch ? origFetch(input, init) : new Response();
}) as typeof fetch;

const workerFile = data.workerFile;
const code = fs.readFileSync(workerFile, "utf8");
// Wrap the classic worker script in a CJS-shaped IIFE that injects
// `require` / `__filename` / `__dirname` from the worker context.
// Plain `eval(code)` would fail under Node 26 because the eval'd
// script uses top-level `require(...)` and `import.meta.url` semantics
// inside an ES module make `require` undefined and the source
// "ambiguous". Using `new Function(...)` avoids both: it forces a
// classic-script parser pass and lets us provide `require` explicitly.
const cjsRequire = createRequire(import.meta.url);
const runner = new Function(
  "require",
  "__filename",
  "__dirname",
  "self",
  code + "\n//# sourceURL=" + workerFile,
);
runner(
  cjsRequire,
  workerFile,
  path.dirname(workerFile),
  globalThis,
);
