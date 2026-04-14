// Shared Node-side bootstrap used by every per-generation loader:
//   - web globals (self/window/location/document) so emscripten's web
//     environment-detection branch activates
//   - fetch() shim that resolves file:// paths (undici rejects them)
//   - Worker polyfill backed by node:worker_threads
//   - package.json injection so dynamic import() treats the lib directory as
//     an ESM package
//   - engine factory invocation with wasmBinary + instantiateWasm overrides

import { Worker as NodeWorker } from "node:worker_threads";
import { fileURLToPath, pathToFileURL } from "node:url";
import fs from "node:fs";
import path from "node:path";
import type { LoaderContext } from "../types.ts";

export type ModuleOverrides = Record<string, unknown>;
export type EmscriptenFactory = (
  opts?: ModuleOverrides,
) => Promise<EmscriptenEngine>;

/**
 * The engine object returned by `await factory()`. We only type the members
 * our loaders actually touch — the underlying type is the emscripten
 * EmscriptenModule.
 */
export interface EmscriptenEngine {
  ccall?: (
    name: string,
    returnType: string,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => number;
  postMessage?: (cmd: string) => void;
  terminate?: () => void;
  addMessageListener?: (listener: (line: string) => void) => void;
  FS?: unknown;
}

let webGlobalsInstalled = false;
let mainConsoleTapped = false;

/**
 * Replace `console.log` on the Node main thread with a tee into `sink`.
 * Needed because the emscripten runtime's `out()` function falls back to
 * `console.log` when `Module.print` is ignored (which happens whenever
 * `wasm_pre.js`'s override is overridden back by the generated runtime,
 * starting around emscripten 3.1.60).
 *
 * Idempotent — calling this more than once is a no-op.
 */
export function installMainConsoleTap(sink: (line: string) => void): void {
  if (mainConsoleTapped) return;
  const orig = console.log.bind(console);
  console.log = (...args: unknown[]) => {
    try {
      sink(args.map((a) => String(a)).join(" "));
    } catch {
      orig(...(args as never));
    }
  };
  mainConsoleTapped = true;
}

export function ensureWebGlobals(ctx: LoaderContext): void {
  if (webGlobalsInstalled) return;
  const g = globalThis as Record<string, unknown>;
  if (typeof g.self === "undefined") g.self = globalThis;
  if (typeof g.window === "undefined") g.window = globalThis;
  if (typeof g.location === "undefined") {
    g.location = { href: pathToFileURL(ctx.jsPath).href };
  }
  if (typeof g.document === "undefined") {
    g.document = {
      currentScript: { src: pathToFileURL(ctx.jsPath).href },
      createElement: () => ({}),
    };
  }
  const origFetch = g.fetch as
    | ((input: unknown, init?: unknown) => Promise<Response>)
    | undefined;
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
  webGlobalsInstalled = true;
}

export interface WorkerPolyfillHooks {
  /** Extra workerData keys merged into every spawned worker. */
  extraWorkerData?: Record<string, unknown>;
  /**
   * Called with the raw message payload from a pthread worker, before the
   * engine's own `onmessage` handler runs. Return `true` to consume the
   * message (prevent the engine from seeing it).
   */
  interceptMessage?: (msg: unknown) => boolean | void;
  /**
   * If set, every spawned worker is given its own stdout+stderr pipe (via
   * `{ stdout: true, stderr: true }`) and this callback receives every
   * line printed to either stream. Used to capture pthread-side output
   * that the emscripten runtime emits through `process.stdout.write`
   * (which bypasses any `console.log` override we install inside the
   * worker shim).
   */
  onStdout?: (line: string) => void;
}

export function installWorkerPolyfill(
  shimUrl: URL,
  hooks: WorkerPolyfillHooks = {},
): void {
  const g = globalThis as Record<string, unknown>;

  class WebWorkerPolyfill {
    private _w: NodeWorker;
    onmessage: ((e: { data: unknown }) => void) | null = null;
    onerror: ((e: unknown) => void) | null = null;
    private _messageHandlers: Array<(e: { data: unknown }) => void> = [];
    private _errorHandlers: Array<(e: unknown) => void> = [];

    constructor(target: unknown, webOpts: WorkerOptions = {}) {
      const filepath = resolveWorkerPath(target);
      const wantsStdout = typeof hooks.onStdout === "function";
      const isClassicWorkerFile = filepath.endsWith(".worker.js");

      if (isClassicWorkerFile) {
        // emscripten 3.1.43 style — separate classic script. Route through
        // worker_shim.ts so we can eval the classic script into a faked
        // web-worker scope and forward postMessage in both directions.
        this._w = new NodeWorker(shimUrl, {
          workerData: { workerFile: filepath, ...hooks.extraWorkerData },
          stdout: wantsStdout,
          stderr: wantsStdout,
        });
      } else {
        // emscripten 3.1.60+ style — the "worker" script is the main ES
        // module itself, invoked with workerData = "em-pthread". We must
        // NOT interpose the shim here, because the pthread handshake
        // (shared memory import, transferred WebAssembly.Module, etc.)
        // assumes the worker runs the engine's own runtime code directly.
        // Pass the original web-style options through to NodeWorker — it
        // accepts the same `workerData` / `name` keys, and `type:"module"`
        // is ignored (Node always treats .js as module in worker_threads).
        const nwOpts: Record<string, unknown> = {
          ...(webOpts as unknown as Record<string, unknown>),
          stdout: wantsStdout,
          stderr: wantsStdout,
        };
        this._w = new NodeWorker(
          target instanceof URL ? target : filepath,
          nwOpts as ConstructorParameters<typeof NodeWorker>[1],
        );
      }

      if (wantsStdout && this._w.stdout) {
        pipeLines(this._w.stdout, hooks.onStdout!);
      }
      if (wantsStdout && this._w.stderr) {
        pipeLines(this._w.stderr, (l) => hooks.onStdout!("[stderr] " + l));
      }
      this._w.on("message", (msg: unknown) => {
        let consumed = false;
        if (hooks.interceptMessage) {
          consumed = hooks.interceptMessage(msg) === true;
        }
        if (consumed) return;
        if (typeof this.onmessage === "function") {
          this.onmessage({ data: msg });
        }
        for (const fn of this._messageHandlers) fn({ data: msg });
      });
      this._w.on("error", (err) => {
        if (typeof this.onerror === "function") this.onerror(err);
        for (const fn of this._errorHandlers) fn(err);
      });
    }

    postMessage(msg: unknown, transfer?: readonly Transferable[]): void {
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
  }

  g.Worker = WebWorkerPolyfill as unknown;
}

function pipeLines(
  stream: NodeJS.ReadableStream,
  onLine: (line: string) => void,
): void {
  let buf = "";
  stream.on("data", (chunk: Buffer | string) => {
    buf += chunk.toString("utf8");
    const parts = buf.split("\n");
    buf = parts.pop() ?? "";
    for (const line of parts) {
      if (line.length > 0) onLine(line);
    }
  });
  stream.on("end", () => {
    if (buf.length > 0) {
      onLine(buf);
      buf = "";
    }
  });
}

function resolveWorkerPath(target: unknown): string {
  if (target instanceof URL) {
    return target.protocol === "file:" ? fileURLToPath(target) : String(target);
  }
  if (typeof target === "string") {
    if (target.startsWith("file://")) return fileURLToPath(target);
    return path.resolve(target);
  }
  return path.resolve(String(target));
}

export async function loadFactory(
  ctx: LoaderContext,
): Promise<EmscriptenFactory> {
  const pkgJson = path.join(ctx.libDir, "package.json");
  if (!fs.existsSync(pkgJson)) {
    fs.writeFileSync(pkgJson, JSON.stringify({ type: "module" }) + "\n");
  }
  const mod = await import(pathToFileURL(ctx.jsPath).href);
  return ((mod as { default?: EmscriptenFactory }).default ??
    (mod as unknown as EmscriptenFactory));
}

/**
 * Full bootstrap used by every Node loader: install the worker polyfill
 * with pthread-side stdout tee, instantiate the engine with print/printErr
 * wired into `push`, and subscribe to `addMessageListener` too. Every
 * observable stdout path lands in `push`, so listeners see a merged stream
 * regardless of which tap the engine is actually using on this emscripten
 * version.
 */
export async function instantiateWithUnifiedStdout(
  ctx: LoaderContext,
  shimUrl: URL,
  push: (line: string) => void,
): Promise<EmscriptenEngine> {
  ensureWebGlobals(ctx);
  const tap = (src: string, line: string) => {
    if (process.env.YANEURA_DEBUG_TAP) {
      process.stderr.write(`[tap:${src}] ${line}\n`);
    }
    push(line);
  };
  installMainConsoleTap((line: string) => tap("mainConsole", line));
  installWorkerPolyfill(shimUrl, {
    extraWorkerData: { teeStdout: true },
    onStdout: (line: string) => tap("stdout", line),
    interceptMessage: (msg) => {
      const data = msg as { __yaneurao_stdout?: unknown; text?: unknown } | null;
      if (data && typeof data === "object" && data.__yaneurao_stdout) {
        tap("msg", String(data.text));
        return true;
      }
      return false;
    },
  });
  const engine = await instantiateEngine(ctx, {
    print: (s: string) => tap("print", s),
    printErr: (s: string) => tap("print", "[err] " + s),
  });
  if (typeof engine.addMessageListener === "function") {
    engine.addMessageListener((s: string) => tap("addMsg", s));
  }
  return engine;
}

export async function instantiateEngine(
  ctx: LoaderContext,
  overrides: ModuleOverrides,
): Promise<EmscriptenEngine> {
  const factory = await loadFactory(ctx);
  const wasmBinary = fs.readFileSync(ctx.wasmPath);
  const baseOpts: ModuleOverrides = {
    wasmBinary,
    mainScriptUrlOrBlob: pathToFileURL(ctx.jsPath).href,
    locateFile: (p: string) => path.join(ctx.libDir, p),
    instantiateWasm: (
      imports: WebAssembly.Imports,
      receiveInstance: (
        instance: WebAssembly.Instance,
        module: WebAssembly.Module,
      ) => void,
    ) => {
      WebAssembly.instantiate(wasmBinary, imports).then(
        ({ instance, module }) => {
          receiveInstance(instance, module);
        },
      );
      return {};
    },
  };
  return factory({ ...baseOpts, ...overrides });
}
