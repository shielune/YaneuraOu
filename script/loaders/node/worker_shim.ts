// worker_threads shim that emulates a web-worker environment for the
// emscripten-generated pthread entry points.
//
// Handles two source shapes produced by different emscripten generations:
//   - classic `*.worker.js` (3.1.43): eval'd into the worker's global scope
//   - ES-module main `.js` reused as pthread entry (3.1.60+): dynamic import
//
// When `workerData.teeStdout` is true, console.log is rerouted to
// `parentPort.postMessage({ __yaneurao_stdout, text })` so the ccall-only
// loader can collect pthread-side stdout.

import { parentPort, workerData } from "node:worker_threads";
import fs from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";

const data = workerData as { workerFile: string; teeStdout?: boolean };
const g = globalThis as any;

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

g.postMessage = (msg: unknown, transfer?: readonly Transferable[]) =>
  parentPort!.postMessage(msg, transfer as never);

let onmessage: ((e: { data: unknown }) => void) | null = null;
const pending: unknown[] = [];
Object.defineProperty(globalThis, "onmessage", {
  configurable: true,
  get() {
    return onmessage;
  },
  set(fn) {
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

const origFetch = g.fetch;
g.fetch = async (input: unknown, init?: unknown): Promise<Response> => {
  const s =
    typeof input === "string"
      ? input
      : (input as { url?: string })?.url ?? String(input);
  if (s.startsWith("file://")) {
    const buf = fs.readFileSync(fileURLToPath(s));
    return new Response(buf);
  }
  return origFetch ? origFetch(input, init) : new Response();
};

if (data.teeStdout) {
  const origLog = console.log.bind(console);
  const tee = (...args: unknown[]) => {
    try {
      const s = args.map((a) => String(a)).join(" ");
      parentPort!.postMessage({ __yaneurao_stdout: true, text: s });
    } catch {
      origLog(...(args as never));
    }
  };
  console.log = tee;
  console.error = tee;
}

const workerFile = data.workerFile;
if (workerFile.endsWith(".worker.js")) {
  const code = fs.readFileSync(workerFile, "utf8");
  (0, eval)(code);
} else {
  await import(pathToFileURL(workerFile).href);
}
