// Shared browser-side engine bootstrap. Runs inside headless Chromium;
// assumes `crossOriginIsolated` (served by wasm_eval_browser.ts with the
// COOP/COEP headers).

import type { LoaderContext } from "../types.ts";

/**
 * Subset of the emscripten Module object our loaders actually touch.
 */
export interface BrowserEngineAPI {
  ccall?: (
    name: string,
    returnType: string,
    argTypes: readonly string[],
    args: readonly unknown[],
  ) => number;
  postMessage?: (cmd: string) => void;
  addMessageListener?: (listener: (line: string) => void) => void;
  terminate?: () => void;
}

export interface FactoryOverrides {
  print?: (s: string) => void;
  printErr?: (s: string) => void;
  mainScriptUrlOrBlob?: string;
  locateFile?: (p: string) => string;
}

export async function loadEngine(
  ctx: LoaderContext,
  overrides: FactoryOverrides = {},
): Promise<BrowserEngineAPI> {
  if (!crossOriginIsolated) {
    throw new Error(
      "not crossOriginIsolated — SharedArrayBuffer is unavailable; check COOP/COEP headers",
    );
  }
  const mod = await import(/* @vite-ignore */ ctx.jsPath);
  const factory = (mod as { default?: unknown }).default ?? mod;
  const engine = await (
    factory as (opts?: FactoryOverrides) => Promise<BrowserEngineAPI>
  )(overrides);
  return engine;
}

/**
 * Patch `window.Worker` so every pthread worker the engine spawns also
 * delivers pthread-side stdout back to the main thread via
 * `{__yaneurao_stdout, text}` messages. The server-side prelude injected
 * by wasm_eval_browser.ts is what produces those messages — we only
 * listen for them here.
 *
 * Returns a function that pushes an observed stdout string into `sink`,
 * which can be called by other code paths too (e.g. main-thread `print`).
 */
export function installWorkerStdoutTap(
  sink: (line: string) => void,
): void {
  const w = window as unknown as {
    Worker: new (url: string | URL, opts?: WorkerOptions) => Worker;
  };
  const RealWorker = w.Worker;
  class PatchedWorker extends RealWorker {
    constructor(url: string | URL, opts?: WorkerOptions) {
      super(url, opts);
      super.addEventListener("message", (ev: MessageEvent) => {
        const data = ev?.data as
          | { __yaneurao_stdout?: unknown; text?: unknown }
          | null
          | undefined;
        if (data && typeof data === "object" && data.__yaneurao_stdout) {
          sink(String(data.text));
        }
      });
    }
  }
  w.Worker = PatchedWorker as unknown as typeof RealWorker;
}

/**
 * Replace `console.log` on the main thread with a tee into `sink`. Needed
 * for emscripten ≥ 3.1.74, where the minified main JS hard-codes
 * `sa = console.log.bind(console)` and ignores `Module.print`.
 *
 * Idempotent: calling this twice won't stack patches.
 */
let consoleTapInstalled = false;
export function installConsoleTap(sink: (line: string) => void): void {
  if (consoleTapInstalled) return;
  const orig = console.log.bind(console);
  console.log = (...args: unknown[]) => {
    try {
      sink(args.map((a) => String(a)).join(" "));
    } catch {
      orig(...(args as never));
    }
  };
  consoleTapInstalled = true;
}

/**
 * Full bootstrap used by every browser loader: install both stdout taps,
 * then load the engine with print/printErr wired into `push`, and subscribe
 * to `addMessageListener` too. Every observable stdout path lands in `push`
 * so listeners see a merged stream regardless of which tap the engine uses
 * on this emscripten version.
 */
export async function loadEngineWithUnifiedStdout(
  ctx: LoaderContext,
  push: (line: string) => void,
): Promise<BrowserEngineAPI> {
  installConsoleTap(push);
  installWorkerStdoutTap(push);
  const engine = await loadEngine(ctx, {
    print: (s: string) => push(s),
    printErr: (s: string) => push("[err] " + s),
  });
  if (typeof engine.addMessageListener === "function") {
    engine.addMessageListener(push);
  }
  return engine;
}
