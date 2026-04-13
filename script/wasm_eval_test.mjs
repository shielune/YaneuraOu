#!/usr/bin/env node
// Load a WASM-built YaneuraOu engine in Node, feed a position, and print the eval.
//
// The build is `-s EXPORT_ES6=1 -s ENVIRONMENT=web,worker` (pthread-enabled),
// so we polyfill `globalThis.Worker` with a Node worker_threads shim.
//
// Usage:
//   node script/wasm_eval_test.mjs <path-to-yaneuraou.<pkg>.js> [--think-ms 30000]

import fs from "node:fs";
import path from "node:path";
import url from "node:url";
import { Worker as NodeWorker } from "node:worker_threads";

const SFEN = "lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1";
const DEFAULT_THINK_MS = 30000;

const argv = process.argv.slice(2);
if (argv.length < 1) {
  console.error("usage: node script/wasm_eval_test.mjs <yaneuraou.<pkg>.js> [--think-ms N]");
  process.exit(2);
}
const jsPath = path.resolve(argv[0]);
let thinkMs = DEFAULT_THINK_MS;
for (let i = 1; i < argv.length; i++) {
  if (argv[i] === "--think-ms") thinkMs = Number(argv[++i]);
}

if (!fs.existsSync(jsPath)) {
  console.error(`not found: ${jsPath}`);
  process.exit(2);
}
const libDir = path.dirname(jsPath);
const wasmPath = jsPath.replace(/\.js$/, ".wasm");
if (!fs.existsSync(wasmPath)) {
  console.error(`not found: ${wasmPath}`);
  process.exit(2);
}

// Force dynamic import() of the engine's main .js to use ESM resolution.
const pkgJsonPath = path.join(libDir, "package.json");
if (!fs.existsSync(pkgJsonPath)) {
  fs.writeFileSync(pkgJsonPath, JSON.stringify({ type: "module" }) + "\n");
}

// Minimal web globals the emscripten glue touches on the main thread.
// Emscripten checks `"object" == typeof window` to decide this is a main
// browser thread; stubbing window makes the "web" code path activate, which
// is the one that's actually compiled into ENVIRONMENT=web,worker builds.
if (typeof globalThis.self === "undefined") globalThis.self = globalThis;
if (typeof globalThis.window === "undefined") globalThis.window = globalThis;
if (typeof globalThis.location === "undefined") {
  globalThis.location = { href: url.pathToFileURL(jsPath).href };
}
if (typeof globalThis.document === "undefined") {
  globalThis.document = {
    currentScript: { src: url.pathToFileURL(jsPath).href },
    createElement: () => ({}),
  };
}

// Polyfill `Worker` using node:worker_threads. Emscripten's main JS creates
// workers via `new Worker(new URL("...worker.js", import.meta.url))`.
const shimUrl = new URL("./wasm_eval_worker_shim.mjs", import.meta.url);
class WebWorkerPolyfill {
  constructor(target /*, opts */) {
    const filepath = target instanceof URL
      ? url.fileURLToPath(target)
      : typeof target === "string" && target.startsWith("file://")
        ? url.fileURLToPath(target)
        : path.resolve(target);
    this._w = new NodeWorker(shimUrl, { workerData: { workerFile: filepath } });
    this._w.on("message", (msg) => {
      if (typeof this.onmessage === "function") this.onmessage({ data: msg });
    });
    this._w.on("error", (err) => {
      if (typeof this.onerror === "function") this.onerror(err);
      else console.error("[worker error]", err);
    });
    this._w.on("exit", (code) => {
      if (code !== 0) console.error(`[worker exit code ${code}]`);
    });
  }
  postMessage(msg, transfer) { this._w.postMessage(msg, transfer); }
  terminate() { return this._w.terminate(); }
  addEventListener(type, fn) {
    if (type === "message") this.onmessage = (e) => fn(e);
    else if (type === "error") this.onerror = (e) => fn(e);
  }
  removeEventListener() { /* no-op */ }
}
globalThis.Worker = WebWorkerPolyfill;

const wasmBinary = fs.readFileSync(wasmPath);
const mod = await import(url.pathToFileURL(jsPath).href);
const factory = mod.default ?? mod;

// Polyfill fetch for file:// URLs — newer emscripten uses fetch even when
// wasmBinary is passed, and Node's undici refuses file:// with "not implemented".
const origFetch = globalThis.fetch;
globalThis.fetch = async (input, init) => {
  const s = typeof input === "string" ? input : (input?.url ?? String(input));
  if (s.startsWith("file://")) {
    const buf = fs.readFileSync(url.fileURLToPath(s));
    return new Response(buf);
  }
  return origFetch(input, init);
};

const engine = await factory({
  wasmBinary,
  mainScriptUrlOrBlob: url.pathToFileURL(jsPath).href,
  locateFile: (p) => path.join(libDir, p),
  print: () => {},
  printErr: () => {},
  instantiateWasm: (imports, receiveInstance) => {
    WebAssembly.instantiate(wasmBinary, imports).then(({ instance, module }) => {
      receiveInstance(instance, module);
    });
    return {};
  },
});

const lines = [];
if (typeof engine.addMessageListener === "function") {
  engine.addMessageListener((line) => lines.push(line));
} else {
  console.error("engine.addMessageListener is not a function");
  process.exit(3);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(predicate, timeoutMs) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    if (predicate()) return true;
    await sleep(20);
  }
  return false;
}

engine.postMessage("usi");
await waitFor(() => lines.some((l) => l === "usiok"), 10000);

engine.postMessage("setoption name Threads value 1");
engine.postMessage("setoption name USI_Hash value 64");
engine.postMessage("isready");
await waitFor(() => lines.some((l) => l === "readyok"), 15000);

engine.postMessage(`position sfen ${SFEN}`);

const goStart = lines.length;
engine.postMessage(`go btime 0 wtime 0 byoyomi ${thinkMs}`);

await waitFor(() => lines.slice(goStart).some((l) => l.startsWith("bestmove")), thinkMs + 10000);

const goLines = lines.slice(goStart);
const infos = goLines.filter((l) => l.startsWith("info") && l.includes(" score "));
const lastInfo = infos[infos.length - 1];
const bestmove = goLines.find((l) => l.startsWith("bestmove"));

function extractScore(line) {
  if (!line) return null;
  const m = line.match(/\bscore (cp|mate) (-?\d+)/);
  return m ? { kind: m[1], value: Number(m[2]) } : null;
}

const engineTag = path.basename(path.dirname(libDir)); // build/<ver>/<pkg>/lib -> <pkg>
const versionTag = path.basename(path.dirname(path.dirname(libDir))); // -> <ver>
console.log(JSON.stringify({
  version: versionTag,
  engine: engineTag,
  thinkMs,
  sfen: SFEN,
  score: extractScore(lastInfo),
  bestmove: bestmove ?? null,
  lastInfo: lastInfo ?? null,
  infoCount: infos.length,
}, null, 2));

engine.postMessage("quit");
process.exit(0);
