/**
 * @ultemica/yaneuraou-wasm-node-halfkp768
 *
 * YaneuraOu NNUE KP256 shogi engine for Node.js, built with
 * `EM_ENVIRONMENT=node`, `EM_PTHREAD=1`, and emscripten 3.1.43 — the only
 * toolchain currently confirmed to drive a multi-threaded YaneuraOu Wasm
 * binary correctly under Node. See `docs/wasm_client_usage.md` §4 for the
 * full bootstrap rationale; this loader inlines the documented sequence so
 * consumers can just `import { createEngine }` without touching node:worker_threads.
 *
 * Sister packages:
 *   - `@ultemica/yaneuraou-wasm-pthread-kp256`  — browser pthread variant
 *   - `@ultemica/yaneuraou-wasm-kp256-cfworkers` — single-thread V8 Isolate
 *
 * Eval (NNUE nn.bin) and book files are NOT embedded in the Wasm binary.
 * Pass them as `Uint8Array` / `ArrayBuffer` at runtime via `createEngine()`.
 * The loader writes them into Emscripten's MEMFS so the C++ engine opens
 * them transparently through the standard `EvalDir` / `BookDir` USI options.
 *
 * ## Usage (Node 18+)
 *
 * ```ts
 * import fs from "node:fs/promises";
 * import { createRequire } from "node:module";
 * import { createEngine } from "@ultemica/yaneuraou-wasm-node-halfkp768";
 * import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-node-halfkp768/engine";
 *
 * const require = createRequire(import.meta.url);
 * const wasmPath = require.resolve(
 *   "@ultemica/yaneuraou-wasm-node-halfkp768/wasm",
 * );
 * const enginePath = require.resolve(
 *   "@ultemica/yaneuraou-wasm-node-halfkp768/engine",
 * );
 * const wasmBinary = await fs.readFile(wasmPath);
 * const evalBin = await fs.readFile("./eval/nn.bin");
 *
 * const engine = await createEngine({
 *   factory: YaneuraOuFactory,
 *   enginePath,
 *   wasmBinary,
 *   evalBin,
 *   threads: 4,
 *   usiHash: 64,
 * });
 *
 * const result = await engine.eval({ sfen: "...", byoyomi: 1000 });
 * engine.dispose();
 * ```
 */

import { Worker as NodeWorker } from "node:worker_threads";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// -------------------------------------------------------------------
// Types
// -------------------------------------------------------------------

// `@types/node` does not ship the `WebAssembly` namespace, and we're building
// without the DOM lib (Node-only package), so spell out the slice we touch.
type WasmImports = Record<string, Record<string, unknown>>;
type WasmInstance = unknown;
type WasmModule = unknown;
type WasmInstantiateWasm = (
  imports: WasmImports,
  receiveInstance: (instance: WasmInstance, module: WasmModule) => void,
) => Record<string, unknown>;

export type YaneuraOuFactory = (opts?: {
  wasmBinary?: ArrayBuffer | Uint8Array;
  locateFile?: (path: string) => string;
  mainScriptUrlOrBlob?: string;
  print?: (line: string) => void;
  printErr?: (line: string) => void;
  noInitialRun?: boolean;
  instantiateWasm?: WasmInstantiateWasm;
  [key: string]: unknown;
}) => Promise<YaneuraOuInstance>;

export interface YaneuraOuInstance {
  postMessage(command: string): void;
  addMessageListener(listener: (line: string) => void): void;
  removeMessageListener(listener: (line: string) => void): void;
  terminate(): void;
  callMain?: (args: readonly string[]) => number;
  FS: {
    mkdirTree(path: string): void;
    writeFile(path: string, data: Uint8Array): void;
  };
}

export interface CreateEngineOptions {
  /**
   * The default export from `@ultemica/yaneuraou-wasm-node-halfkp768/engine`.
   * This is the Emscripten module factory.
   */
  factory: YaneuraOuFactory;
  /**
   * Absolute path to `yaneuraou.js` (resolved via
   * `require.resolve("@ultemica/yaneuraou-wasm-node-halfkp768/engine")` in your
   * app). Used as the value of `mainScriptUrlOrBlob` and as the base for
   * `locateFile` so emscripten resolves the sibling `yaneuraou.worker.js`
   * to the same directory.
   *
   * If omitted, the loader falls back to its own `dist/` location, which
   * works for the standard npm-install layout but not for vendored copies.
   */
  enginePath?: string;
  /** The WASM bytes (typically `await fs.readFile(.../yaneuraou.wasm)`). */
  wasmBinary: ArrayBuffer | Uint8Array;
  /** NNUE eval file contents. */
  evalBin?: ArrayBuffer | Uint8Array;
  /** Eval file name written to MEMFS. Default `"nn.bin"`. */
  evalFile?: string;
  /** Opening book contents (optional). */
  bookDb?: ArrayBuffer | Uint8Array;
  /** Book file name. Required when `bookDb` is provided. */
  bookFile?: string;
  /** Hash table size in MB. Default `64`. */
  usiHash?: number;
  /**
   * `Threads` USI option. Default `1`. Increase for parallel search; the
   * engine was built with `PTHREAD_POOL_SIZE=32`, so values up to 32 are
   * accepted without a relink.
   */
  threads?: number;
  /** `usi` handshake timeout in ms. Default `10_000`. */
  handshakeTimeoutMs?: number;
  /** `isready` timeout in ms. Default `30_000`. */
  readyTimeoutMs?: number;
}

export interface EvalRequest {
  sfen: string;
  byoyomi?: number;
  skillLevel?: number;
  multiPv?: number;
  depthLimit?: number;
  nodesLimit?: number;
}

export type Score = {
  kind: "cp" | "mate";
  value: number;
  bound?: "lowerbound" | "upperbound";
};

export interface EvalResult {
  bestmove: string;
  ponder: string | null;
  score: Score | null;
  depth: number | null;
  seldepth: number | null;
  nodes: number | null;
  timeMs: number | null;
  pv: string[] | null;
  lastInfo: string | null;
  bestmoveLine: string;
}

export interface Engine {
  send(command: string): void;
  eval(req: EvalRequest): Promise<EvalResult>;
  evalBatch(reqs: EvalRequest[]): Promise<EvalResult[]>;
  dispose(): void;
}

// -------------------------------------------------------------------
// Node bootstrap — install web globals + Worker polyfill before the
// emscripten factory runs.
//
// Why: emscripten 3.1.43's pthread runtime, even when built with
// `EM_ENVIRONMENT=node`, fires `new Worker(URL("yaneuraou.worker.js",
// import.meta.url), {type:"module"})` at pthread-spawn time. Node's ESM
// `import.meta.url` works fine, but the *worker target itself*
// (`yaneuraou.worker.js`) is a classic script with top-level `require(...)`
// — when Node tries to load it through worker_threads as an ES module, the
// `require` symbol is undefined and the worker crashes immediately.
//
// The fix is to replace `globalThis.Worker` with our own class that maps
// the worker URL onto `node:worker_threads.Worker` plus a small shim
// (`worker_shim.js`) running inside each worker that eval()'s the classic
// `.worker.js` into a faked web-worker global scope. The same trick
// `script/loaders/node/common.ts` uses for the eval-harness; this package
// inlines the minimum needed at runtime so consumers get a single
// `createEngine` call with no extra plumbing.
// -------------------------------------------------------------------

let webGlobalsInstalled = false;
let workerPolyfillInstalled = false;

function ensureWebGlobals(jsHref: string): void {
  if (webGlobalsInstalled) return;
  const g = globalThis as Record<string, unknown>;
  if (typeof g.self === "undefined") g.self = globalThis;
  if (typeof g.window === "undefined") g.window = globalThis;
  if (typeof g.location === "undefined") g.location = { href: jsHref };
  if (typeof g.document === "undefined") {
    g.document = {
      currentScript: { src: jsHref },
      createElement: () => ({}),
    };
  }
  // emscripten 3.1.43 paths use fetch(file://) to read the wasm and the
  // worker script. Node's undici rejects file:// — fall through to a
  // streaming-friendly Response built from readFile.
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
  webGlobalsInstalled = true;
}

function installWorkerPolyfill(shimUrl: URL): void {
  if (workerPolyfillInstalled) return;
  const g = globalThis as Record<string, unknown>;

  class NodeWorkerPolyfill {
    private _w: NodeWorker;
    onmessage: ((e: { data: unknown }) => void) | null = null;
    onerror: ((e: unknown) => void) | null = null;
    private _messageHandlers: Array<(e: { data: unknown }) => void> = [];
    private _errorHandlers: Array<(e: unknown) => void> = [];

    constructor(target: unknown, _opts: Record<string, unknown> = {}) {
      const filepath = resolveWorkerPath(target);
      const isClassicWorker = filepath.endsWith(".worker.js");

      if (isClassicWorker) {
        // emscripten 3.1.43 style — route through the shim so we can eval
        // the classic .worker.js script into a faked web-worker scope.
        this._w = new NodeWorker(shimUrl, {
          workerData: { workerFile: filepath },
        });
      } else {
        // ES-module-style worker (3.1.60+ / unlikely under 3.1.43 but
        // kept for forward-compatibility) — let Node load it directly.
        this._w = new NodeWorker(
          target instanceof URL ? target : filepath,
        );
      }

      this._w.on("message", (msg: unknown) => {
        if (typeof this.onmessage === "function") {
          this.onmessage({ data: msg });
        }
        for (const fn of this._messageHandlers) fn({ data: msg });
      });
      this._w.on("error", (err: unknown) => {
        if (typeof this.onerror === "function") this.onerror(err);
        for (const fn of this._errorHandlers) fn(err);
      });
    }

    postMessage(msg: unknown, transfer?: readonly unknown[]): void {
      this._w.postMessage(msg, transfer as never);
    }

    terminate(): Promise<number> {
      return this._w.terminate();
    }

    addEventListener(type: string, fn: (e: unknown) => void): void {
      if (type === "message") {
        this._messageHandlers.push(fn as (e: { data: unknown }) => void);
      } else if (type === "error") {
        this._errorHandlers.push(fn);
      }
    }

    removeEventListener(): void {
      /* no-op */
    }

    // emscripten's node-environment runtime treats Worker as a Node
    // EventEmitter and calls `.on("message", ...)` / `.on("error", ...)`
    // directly. Forward those to the underlying NodeWorker so the engine's
    // pthread bootstrap can register its handlers the way it expects.
    on(event: string, fn: (...args: unknown[]) => void): this {
      this._w.on(event, fn);
      return this;
    }
    off(event: string, fn: (...args: unknown[]) => void): this {
      this._w.off(event, fn);
      return this;
    }
    once(event: string, fn: (...args: unknown[]) => void): this {
      this._w.once(event, fn);
      return this;
    }

    // Node's worker_threads.Worker also exposes `ref()`/`unref()`/`threadId`
    // which some emscripten paths touch — forward them for symmetry.
    ref(): void {
      this._w.ref();
    }
    unref(): void {
      this._w.unref();
    }
    get threadId(): number {
      return this._w.threadId;
    }
  }

  // emscripten's node-environment runtime executes
  //     global.Worker = nodeWorkerThreads.Worker;
  // inside the factory body, which would clobber a plain assignment to
  // globalThis.Worker. Install the polyfill as an accessor whose setter
  // is a no-op, so the engine's reassignment is silently ignored and
  // every `new Worker(...)` callsite later in the engine still resolves
  // to our polyfill class.
  const Polyfill: unknown = NodeWorkerPolyfill;
  Object.defineProperty(g, "Worker", {
    configurable: true,
    get() {
      return Polyfill;
    },
    set() {
      /* engine tries to overwrite with native — ignore */
    },
  });
  workerPolyfillInstalled = true;
}

function resolveWorkerPath(target: unknown): string {
  if (target instanceof URL) {
    return target.protocol === "file:"
      ? fileURLToPath(target)
      : String(target);
  }
  if (typeof target === "string") {
    if (target.startsWith("file://")) return fileURLToPath(target);
    return path.resolve(target);
  }
  return path.resolve(String(target));
}

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

export async function createEngine(opts: CreateEngineOptions): Promise<Engine> {
  const usiHash = opts.usiHash ?? 64;
  const threads = opts.threads ?? 1;
  const handshakeTimeout = opts.handshakeTimeoutMs ?? 10_000;
  const readyTimeout = opts.readyTimeoutMs ?? 30_000;

  // Resolve the engine js path. If the consumer didn't pass enginePath,
  // assume the canonical npm layout where this loader (dist/index.js) sits
  // next to dist/yaneuraou.js / dist/yaneuraou.worker.js.
  const engineDir = opts.enginePath
    ? path.dirname(opts.enginePath)
    : path.dirname(fileURLToPath(import.meta.url));
  const enginePath = opts.enginePath ?? path.join(engineDir, "yaneuraou.js");
  const shimPath = path.join(engineDir, "worker_shim.js");
  if (!fs.existsSync(shimPath)) {
    throw new Error(
      `yaneuraou-wasm-node-halfkp768: worker_shim.js not found at ${shimPath}. ` +
        "The package must be installed via npm so dist/yaneuraou.{js,wasm,worker.js} " +
        "and dist/worker_shim.js all live in the same directory.",
    );
  }
  const jsHref = pathToFileURL(enginePath).href;

  ensureWebGlobals(jsHref);
  installWorkerPolyfill(pathToFileURL(shimPath));

  const lines: string[] = [];
  const wasmBinary =
    opts.wasmBinary instanceof Uint8Array
      ? opts.wasmBinary
      : new Uint8Array(opts.wasmBinary);

  const factoryArgs: Parameters<YaneuraOuFactory>[0] = {
    wasmBinary,
    mainScriptUrlOrBlob: jsHref,
    locateFile: (p: string) => path.join(engineDir, p),
    print: (line: string) => {
      lines.push(line);
      if (process.env.YANEURA_DEBUG) process.stderr.write(`[stdout] ${line}\n`);
    },
    printErr: (line: string) => {
      lines.push(line);
      if (process.env.YANEURA_DEBUG) process.stderr.write(`[stderr] ${line}\n`);
    },
    // YaneuraOu's main() under __EMSCRIPTEN__ runs Options/NNUE/Threads
    // init and then returns. On Node, letting it run automatically
    // deadlocks the subsequent ccall path; suppress it and trigger
    // callMain() manually after the factory resolves.
    noInitialRun: true,
    instantiateWasm: ((
      imports: WasmImports,
      receiveInstance: (instance: WasmInstance, module: WasmModule) => void,
    ) => {
      // `WebAssembly` is a global at runtime in Node; we don't ship its
      // TS types here so cast through `any`-equivalent locally.
      const wa = (globalThis as { WebAssembly?: unknown }).WebAssembly as {
        instantiate(
          bytes: Uint8Array,
          imports: WasmImports,
        ): Promise<{ instance: WasmInstance; module: WasmModule }>;
      };
      wa.instantiate(wasmBinary, imports).then(({ instance, module }) => {
        receiveInstance(instance, module);
      });
      return {};
    }) satisfies WasmInstantiateWasm,
  };

  const instance = await opts.factory(factoryArgs);
  instance.addMessageListener((line) => {
    lines.push(line);
  });

  if (typeof instance.callMain === "function") {
    try {
      instance.callMain([]);
    } catch (e) {
      const err = e as { name?: string };
      if (err && err.name !== "ExitStatus") throw e;
    }
  } else {
    throw new Error(
      "yaneuraou-wasm-node-halfkp768: engine.callMain is not exposed — the " +
        "Wasm binary was not built with EXPORTED_RUNTIME_METHODS including " +
        "'callMain'. Rebuild via .github/workflows/build-wasm.yml.",
    );
  }

  if (opts.evalBin) {
    const evalFile = opts.evalFile ?? "nn.bin";
    instance.FS.mkdirTree("/eval");
    instance.FS.writeFile(`/eval/${evalFile}`, toBytes(opts.evalBin));
  }
  if (opts.bookDb && opts.bookFile) {
    instance.FS.mkdirTree("/book");
    instance.FS.writeFile(`/book/${opts.bookFile}`, toBytes(opts.bookDb));
  }

  await sendAndWait(
    instance,
    "usi",
    () => lines.includes("usiok"),
    handshakeTimeout,
    "usi -> usiok timeout",
  );

  instance.postMessage(`setoption name Threads value ${threads}`);
  instance.postMessage(`setoption name USI_Hash value ${usiHash}`);
  instance.postMessage(`setoption name Hash value ${usiHash}`);

  if (opts.evalBin) {
    instance.postMessage("setoption name EvalDir value /eval");
  }
  if (opts.bookDb && opts.bookFile) {
    instance.postMessage("setoption name BookDir value /book");
    instance.postMessage(`setoption name BookFile value ${opts.bookFile}`);
  }

  const initMark = lines.length;
  await sendAndWait(
    instance,
    "isready",
    () => lines.slice(initMark).includes("readyok"),
    readyTimeout,
    "initial isready -> readyok timeout",
  );

  let chain: Promise<unknown> = Promise.resolve();
  const serial = <T>(fn: () => Promise<T>): Promise<T> => {
    const next = chain.then(fn, fn);
    chain = next.catch(() => undefined);
    return next;
  };

  function applyRequestOptions(req: EvalRequest): void {
    instance.postMessage(`setoption name MultiPV value ${req.multiPv ?? 1}`);
    instance.postMessage(
      `setoption name SkillLevel value ${req.skillLevel ?? 20}`,
    );
    instance.postMessage(
      `setoption name DepthLimit value ${req.depthLimit ?? 0}`,
    );
    instance.postMessage(
      `setoption name NodesLimit value ${req.nodesLimit ?? 0}`,
    );
  }

  async function runOnePosition(req: EvalRequest): Promise<EvalResult> {
    instance.postMessage(`position sfen ${req.sfen}`);
    const movetime = req.byoyomi ?? 500;
    const goMark = lines.length;
    instance.postMessage(`go movetime ${movetime}`);
    const ok = await waitFor(
      () => lines.slice(goMark).some((l) => l.startsWith("bestmove")),
      movetime + 30_000,
    );
    if (!ok) {
      throw new Error(`go movetime ${movetime} -> bestmove timeout`);
    }
    return parseGoResult(lines.slice(goMark));
  }

  async function evalPosition(req: EvalRequest): Promise<EvalResult> {
    return serial(async () => {
      applyRequestOptions(req);
      instance.postMessage("usinewgame");
      const readyMark = lines.length;
      await sendAndWait(
        instance,
        "isready",
        () => lines.slice(readyMark).includes("readyok"),
        readyTimeout,
        "eval isready -> readyok timeout",
      );
      const result = await runOnePosition(req);
      lines.length = 0;
      return result;
    });
  }

  async function evalBatchPositions(
    reqs: EvalRequest[],
  ): Promise<EvalResult[]> {
    if (reqs.length === 0) return [];
    return serial(async () => {
      applyRequestOptions(reqs[0]!);
      instance.postMessage("usinewgame");
      const readyMark = lines.length;
      await sendAndWait(
        instance,
        "isready",
        () => lines.slice(readyMark).includes("readyok"),
        readyTimeout,
        "evalBatch isready -> readyok timeout",
      );
      const results: EvalResult[] = [];
      for (const req of reqs) {
        results.push(await runOnePosition(req));
      }
      lines.length = 0;
      return results;
    });
  }

  function dispose(): void {
    try {
      instance.terminate();
    } catch {
      /* ignore */
    }
  }

  return {
    send: (cmd: string) => instance.postMessage(cmd),
    eval: evalPosition,
    evalBatch: evalBatchPositions,
    dispose,
  };
}

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

function toBytes(data: ArrayBuffer | Uint8Array): Uint8Array {
  return data instanceof Uint8Array ? data : new Uint8Array(data);
}

const sleep = (ms: number): Promise<void> =>
  new Promise((r) => setTimeout(r, ms));

async function waitFor(
  pred: () => boolean,
  timeoutMs: number,
): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    if (pred()) return true;
    await sleep(1);
  }
  return false;
}

async function sendAndWait(
  instance: YaneuraOuInstance,
  command: string,
  pred: () => boolean,
  timeoutMs: number,
  errorMessage: string,
): Promise<void> {
  instance.postMessage(command);
  if (!(await waitFor(pred, timeoutMs))) {
    throw new Error(errorMessage);
  }
}

function parseGoResult(goLines: string[]): EvalResult {
  const bestmoveLine =
    goLines.find((l) => l.startsWith("bestmove")) ?? "bestmove resign";
  const bm = bestmoveLine.match(
    /^bestmove\s+(\S+)(?:\s+ponder\s+(\S+))?/,
  );
  const bestmove = bm?.[1] ?? "resign";
  const ponder = bm?.[2] ?? null;

  const infos = goLines.filter(
    (l) => l.startsWith("info") && l.includes(" score "),
  );
  const lastInfo = infos[infos.length - 1] ?? null;

  let score: Score | null = null;
  let depth: number | null = null;
  let seldepth: number | null = null;
  let nodes: number | null = null;
  let timeMs: number | null = null;
  let pv: string[] | null = null;

  if (lastInfo) {
    const sm = lastInfo.match(
      /\bscore (cp|mate) (-?\d+)(?:\s+(lowerbound|upperbound))?/,
    );
    if (sm) {
      score = {
        kind: sm[1] as "cp" | "mate",
        value: Number(sm[2]),
        ...(sm[3] ? { bound: sm[3] as "lowerbound" | "upperbound" } : {}),
      };
    }
    const dm = lastInfo.match(/\bdepth (\d+)/);
    if (dm) depth = Number(dm[1]);
    const sdm = lastInfo.match(/\bseldepth (\d+)/);
    if (sdm) seldepth = Number(sdm[1]);
    const nm = lastInfo.match(/\bnodes (\d+)/);
    if (nm) nodes = Number(nm[1]);
    const tm = lastInfo.match(/\btime (\d+)/);
    if (tm) timeMs = Number(tm[1]);
    const pvm = lastInfo.match(/\bpv\s+(.+)$/);
    if (pvm) pv = pvm[1].trim().split(/\s+/);
  }

  return {
    bestmove,
    ponder,
    score,
    depth,
    seldepth,
    nodes,
    timeMs,
    pv,
    lastInfo,
    bestmoveLine,
  };
}
