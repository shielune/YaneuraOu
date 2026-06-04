// Shared types for the version-specific engine loaders.
//
// A Loader takes a built yaneuraou.<pkg>.js + sibling .wasm, instantiates the
// engine in the target environment (Node via worker_threads, or browser via
// Playwright+Chromium), and returns an EngineInstance with a uniform USI
// command surface — hiding whether the generation uses Module.postMessage,
// ccall, classic web workers, or ES-module workers.

export interface EngineInstance {
  /**
   * Send a USI command to the engine. Implementations may queue, throttle,
   * or retry internally (ccall on this engine can return 1 = "busy, try
   * later again", so loaders implement an internal backoff loop). The
   * returned promise resolves once the command has been accepted — it
   * does NOT wait for the command's response. Use `onLine` for that.
   */
  sendCommand(cmd: string): Promise<void>;

  /**
   * Register a listener that receives every stdout line the engine emits
   * (both main-thread and pthread-worker origins are merged).
   */
  onLine(listener: (line: string) => void): void;

  /**
   * Tear down the engine: terminate pthread workers, close the main module,
   * release any filesystem handles. Must be safe to call multiple times.
   */
  dispose(): Promise<void>;
}

export interface LoaderContext {
  /** "node" when driven by wasm_eval_node.ts, "browser" when in Chromium. */
  environment: "node" | "browser";

  /** Absolute path to the `build/<ver>_<arch>/<pkg>/lib` directory. */
  libDir: string;

  /** Absolute path to `yaneuraou.<pkg>.js` inside libDir. */
  jsPath: string;

  /** Absolute path to the sibling `yaneuraou.<pkg>.wasm`. */
  wasmPath: string;

  /** The emscripten version string extracted from the build path. */
  emscriptenVersion: string;
}

export interface EngineOptions {
  /** Value for `setoption name Threads`. Defaults to 1. */
  threads?: number;
  /** Value for `setoption name USI_Hash`. Defaults to 64. */
  usiHash?: number;
}

export interface Loader {
  /** Short human-readable name, e.g. "classic-worker/node". */
  readonly name: string;
  /**
   * True if this loader can handle the given emscripten version. Loaders
   * are tried in priority order (see `detect.ts`).
   */
  matches(version: string): boolean;
  /**
   * Instantiate the engine. Implementations must not start the USI flow —
   * the caller owns that — they only need to produce a live `EngineInstance`.
   */
  load(ctx: LoaderContext, opts?: EngineOptions): Promise<EngineInstance>;
}
