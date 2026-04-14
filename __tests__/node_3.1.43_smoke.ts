/**
 * Minimal smoke repro for the 3.1.43 node variant — this is the
 * baseline that's expected to work end-to-end with the classic-worker
 * generation (separate `.worker.js`, wasm_pre.js queue/drain still
 * cooperating with emscripten's lifecycle).
 *
 * Expected output (head): 'id name YaneuraOu NNUE KP256 ...' on the
 * `[aml]` stream, ending with 'usiok'.
 *
 * Run:
 *   bun __tests__/node_3.1.43_smoke.ts
 */

import fs from "node:fs";
import path from "node:path";
import url from "node:url";
import { Worker as NodeWorker } from "node:worker_threads";

const jsPath = path.resolve("build/3.1.43_x86_64/k-p/node/lib/yaneuraou.k-p.js");
if (!fs.existsSync(jsPath)) {
  console.error(`[test] not found: ${jsPath}`);
  console.error("[test] build the 3.1.43 node variant first:");
  console.error(
    "       docker run --rm -v <host>:/src emscripten/emsdk:3.1.43 " +
      "node script/wasm_build.js k-p",
  );
  process.exit(2);
}

const libDir = path.dirname(jsPath);
if (!fs.existsSync(path.join(libDir, "package.json"))) {
  fs.writeFileSync(path.join(libDir, "package.json"), '{"type":"module"}\n');
}

const g = globalThis as unknown as Record<string, unknown>;
g.self ??= globalThis;
g.window ??= globalThis;
g.location ??= { href: url.pathToFileURL(jsPath).href };
g.document ??= {
  currentScript: { src: url.pathToFileURL(jsPath).href },
  createElement: () => ({}),
};
const origFetch = g.fetch as
  | ((input: unknown, init?: unknown) => Promise<Response>)
  | undefined;
g.fetch = async (input: unknown, init?: unknown): Promise<Response> => {
  const s =
    typeof input === "string"
      ? input
      : (input as { url?: string })?.url ?? String(input);
  if (s.startsWith("file://")) {
    return new Response(fs.readFileSync(url.fileURLToPath(s)));
  }
  return origFetch ? origFetch(input, init) : new Response();
};

// Minimal Worker polyfill: classic .worker.js eval'd by an inline shim.
const shimSource = `
const { parentPort, workerData } = require("node:worker_threads");
const fs = require("node:fs");
globalThis.self = globalThis;
globalThis.name = "em-pthread";
globalThis.importScripts = () => {};
globalThis.postMessage = (m) => parentPort.postMessage(m);
let onmessage = null;
Object.defineProperty(globalThis, "onmessage", {
  configurable: true,
  get() { return onmessage; },
  set(fn) { onmessage = fn; },
});
parentPort.on("message", (m) => onmessage?.({ data: m }));
const file = workerData.workerFile;
if (file.endsWith(".worker.js")) {
  (0, eval)(fs.readFileSync(file, "utf8"));
} else {
  import(require("node:url").pathToFileURL(file).href);
}
`;
g.Worker = class {
  _w: NodeWorker;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  constructor(target: unknown) {
    const file =
      target instanceof URL
        ? url.fileURLToPath(target)
        : path.resolve(String(target));
    this._w = new NodeWorker(shimSource, {
      eval: true,
      workerData: { workerFile: file },
    });
    this._w.on("message", (m) => this.onmessage?.({ data: m }));
  }
  postMessage(m: unknown): void {
    this._w.postMessage(m);
  }
  terminate(): Promise<number> {
    return this._w.terminate();
  }
  addEventListener(type: string, fn: (e: unknown) => void): void {
    if (type === "message") this.onmessage = (e) => fn(e);
  }
  removeEventListener(): void {
    /* no-op */
  }
} as unknown;

const mod = await import(url.pathToFileURL(jsPath).href);
const factory = (mod as { default?: (opts?: unknown) => Promise<unknown> })
  .default ?? (mod as unknown as (opts?: unknown) => Promise<unknown>);
const wasmBinary = fs.readFileSync(jsPath.replace(/\.js$/, ".wasm"));

const engine = (await factory({
  wasmBinary,
  mainScriptUrlOrBlob: url.pathToFileURL(jsPath).href,
  locateFile: (p: string) => path.join(libDir, p),
  instantiateWasm: (
    imports: WebAssembly.Imports,
    receiveInstance: (
      i: WebAssembly.Instance,
      m: WebAssembly.Module,
    ) => void,
  ) => {
    WebAssembly.instantiate(wasmBinary, imports).then(
      ({ instance, module }) => receiveInstance(instance, module),
    );
    return {};
  },
})) as {
  addMessageListener?: (l: (line: string) => void) => void;
  postMessage?: (cmd: string) => void;
  terminate?: () => void;
};

const lines: string[] = [];
engine.addMessageListener?.((line) => {
  lines.push(line);
  console.error("[aml]", line);
});

console.error("[test] sending usi");
engine.postMessage?.("usi");

await new Promise<void>((r) => setTimeout(r, 2000));
console.error("[test] lines received:", lines.length);
console.error("[test] contains usiok:", lines.includes("usiok"));
process.exit(lines.includes("usiok") ? 0 : 1);
