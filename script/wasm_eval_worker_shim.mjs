// Shim executed inside a Node worker_threads.Worker to emulate a web worker
// environment for emscripten-generated engine code.
//
// Handles two shapes produced by different emscripten versions:
//   - classic separate worker.js  (3.1.43): `self.onmessage = handleMessage;`
//   - ES-module main .js reused as pthread worker (3.1.60+): detected via
//     `typeof importScripts === 'function' && self.name === 'em-pthread'`
//
// The detection discriminator is the worker file path we were given:
// a `*.worker.js` is a classic script; any other `*.js` is an ES module.

import { parentPort, workerData } from "node:worker_threads";
import fs from "node:fs";
import url from "node:url";

// ---- common web-worker-ish globals ----
globalThis.self = globalThis;
// emscripten 3.1.60+ checks `"em-pthread" == self.name` to decide it's a pthread worker.
globalThis.name = "em-pthread";
// Verify — Node's worker_threads globals are sometimes locked; fall through if not.
if (globalThis.name !== "em-pthread") {
  try { Object.defineProperty(globalThis, "name", { value: "em-pthread", writable: true, configurable: true }); } catch (e) {
    console.error("[shim] could not set globalThis.name:", e);
  }
}
// emscripten 3.1.60+ also uses `typeof importScripts === 'function'` to decide it's a Worker.
globalThis.importScripts = () => {};
// Some versions read self.location.href to resolve relative asset paths.
globalThis.location = { href: url.pathToFileURL(workerData.workerFile).href };
console.error(`[shim pid=${process.pid}] name=${globalThis.name} importScripts=${typeof globalThis.importScripts} hasSelf=${globalThis.self === globalThis}`);

globalThis.postMessage = (msg, transfer) => parentPort.postMessage(msg, transfer);
let _onmessage = null;
const _pending = [];
Object.defineProperty(globalThis, "onmessage", {
  configurable: true,
  get() { return _onmessage; },
  set(fn) {
    _onmessage = fn;
    // Drain any messages that arrived before the handler was installed.
    if (fn && _pending.length) {
      const drained = _pending.splice(0);
      for (const m of drained) fn({ data: m });
    }
  },
});
Object.defineProperty(globalThis, "onunhandledrejection", {
  configurable: true,
  writable: true,
  value: null,
});

parentPort.on("message", (msg) => {
  if (_onmessage) _onmessage({ data: msg });
  else _pending.push(msg);
});

// Polyfill fetch for file:// URLs so the emscripten worker can load its
// sibling .wasm without going through undici (which rejects the scheme).
const origFetch = globalThis.fetch;
globalThis.fetch = async (input, init) => {
  const s = typeof input === "string" ? input : (input?.url ?? String(input));
  if (s.startsWith("file://")) {
    const buf = fs.readFileSync(url.fileURLToPath(s));
    return new Response(buf);
  }
  return origFetch(input, init);
};

// ---- load the worker entry ----
const workerFile = workerData.workerFile;
if (workerFile.endsWith(".worker.js")) {
  // Classic web worker script: eval in global scope.
  const code = fs.readFileSync(workerFile, "utf8");
  (0, eval)(code);
} else {
  // ES module: dynamic import. The module's top-level code is expected to
  // install an onmessage handler once it detects pthread context.
  await import(url.pathToFileURL(workerFile).href);
}
