/**
 * @ultemica/yaneuraou-wasm-mate-cfworkers
 *
 * YaneuraOu mate (tsume) solver for Cloudflare Workers / browser.
 *
 * Single-threaded WASM build (`EM_PTHREAD=0`). Runs anywhere a normal
 * fetch handler runs — no SharedArrayBuffer, no COOP/COEP headers.
 *
 * No eval / book files. The mate engine searches by DfPn and only needs a
 * small hash table (`USI_Hash`, 1MB is plenty for problems up to ~15 ply).
 *
 * The Wasm binary is built with `--pre-js wasm_pre.js`, which exposes:
 *   - `instance.postMessage(cmd)` — queue a USI command
 *   - `instance.addMessageListener(fn)` — subscribe to engine stdout
 *   - `instance.removeMessageListener(fn)`
 *   - `instance.terminate()`
 *
 * ## Usage (browser)
 *
 * ```ts
 * import { createMateEngine } from "@ultemica/yaneuraou-wasm-mate-cfworkers";
 * import YaneuraOuFactory from "@ultemica/yaneuraou-wasm-mate-cfworkers/engine";
 * import wasmUrl from "@ultemica/yaneuraou-wasm-mate-cfworkers/wasm?url";
 *
 * const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
 * const engine = await createMateEngine({ factory: YaneuraOuFactory, wasmBinary });
 *
 * const result = await engine.solve({ sfen: "...", nodesLimit: 100_000 });
 * // result.status === "mate"
 * // result.moves === ["B*5g", "4h5h", "7i6i"]
 * ```
 */

// -------------------------------------------------------------------
// Types
// -------------------------------------------------------------------

/** Emscripten module factory returned by `import YaneuraOu from "…/yaneuraou.js"`. */
export type YaneuraOuFactory = (opts?: {
  wasmBinary?: ArrayBuffer | Uint8Array;
  locateFile?: (path: string) => string;
  print?: (line: string) => void;
  printErr?: (line: string) => void;
  [key: string]: unknown;
}) => Promise<YaneuraOuInstance>;

/** Runtime instance produced by the factory. `wasm_pre.js` injects the USI bridge. */
export interface YaneuraOuInstance {
  postMessage(command: string): void;
  addMessageListener(listener: (line: string) => void): void;
  removeMessageListener(listener: (line: string) => void): void;
  terminate(): void;
  HEAPU8: Uint8Array;
}

export interface CreateMateEngineOptions {
  /** `yaneuraou.js` from `import` or `require`. */
  factory: YaneuraOuFactory;
  /** `.wasm` binary. */
  wasmBinary: ArrayBuffer | Uint8Array;
  /** DfPn hash size in MB. Default `1`. 1MB is enough for typical practical-game tsume. */
  usiHash?: number;
  /** `usi` handshake timeout in ms. Default `5_000`. */
  handshakeTimeoutMs?: number;
  /** `isready` timeout in ms. Default `10_000`. */
  readyTimeoutMs?: number;
}

export interface SolveRequest {
  /** SFEN string (the part after `position sfen`). */
  sfen: string;
  /**
   * Time limit in ms passed as `go mate <ms>`.
   * `0` (default) is treated as no time limit — search runs until either
   * mate is found or `nodesLimit` is hit. Note: in single-threaded WASM the
   * time limit is ignored by the engine; use `nodesLimit` instead.
   */
  byoyomi?: number;
  /** Node limit (0 = unlimited). Default `0`. */
  nodesLimit?: number;
}

export type SolveStatus =
  | "mate" // mating sequence found
  | "nomate" // proven no mate
  | "timeout" // search aborted by time (multi-thread builds only)
  | "none"; // search aborted by NodesLimit / out of memory

export interface SolveResult {
  status: SolveStatus;
  /** Mating sequence (USI moves). Empty unless `status === "mate"`. */
  moves: string[];
  /** Search node count (from the engine's last `info` line). */
  nodes: number | null;
  /** Engine-reported elapsed time in ms (from the engine's last `info time`). */
  timeMs: number | null;
  /** Wall-clock elapsed since the `go mate` command was sent. */
  wallMs: number;
  /** Last `info` line, for debugging. */
  lastInfo: string | null;
}

export interface MateEngine {
  /** Send a raw USI command. */
  send(command: string): void;
  /** Solve a single position. Calls are serialized. */
  solve(req: SolveRequest): Promise<SolveResult>;
  /** Release the engine. Safe to call multiple times. */
  dispose(): void;
}

// -------------------------------------------------------------------
// Implementation
// -------------------------------------------------------------------

export async function createMateEngine(
  opts: CreateMateEngineOptions,
): Promise<MateEngine> {
  const usiHash = opts.usiHash ?? 1;
  const handshakeTimeout = opts.handshakeTimeoutMs ?? 5_000;
  const readyTimeout = opts.readyTimeoutMs ?? 10_000;

  const lines: string[] = [];
  const instance = await opts.factory({ wasmBinary: opts.wasmBinary });
  instance.addMessageListener((line) => {
    lines.push(line);
  });

  await sendAndWait(
    instance,
    "usi",
    () => lines.includes("usiok"),
    handshakeTimeout,
    "usi -> usiok timeout",
  );

  instance.postMessage(`setoption name USI_Hash value ${usiHash}`);

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

  async function solveOne(req: SolveRequest): Promise<SolveResult> {
    instance.postMessage(
      `setoption name NodesLimit value ${req.nodesLimit ?? 0}`,
    );
    instance.postMessage(`position sfen ${req.sfen}`);
    const goMark = lines.length;
    const t0 = nowMs();
    const cmd = req.byoyomi && req.byoyomi > 0
      ? `go mate ${req.byoyomi}`
      : "go mate 0";
    instance.postMessage(cmd);
    const ok = await waitFor(
      () => lines.slice(goMark).some((l) => l.startsWith("checkmate")),
      (req.byoyomi ?? 0) + 60_000,
    );
    const wallMs = nowMs() - t0;
    if (!ok) {
      throw new Error(`go mate -> checkmate timeout (wall ${wallMs}ms)`);
    }
    return parseSolveResult(lines.slice(goMark), wallMs);
  }

  function solve(req: SolveRequest): Promise<SolveResult> {
    return serial(async () => {
      const result = await solveOne(req);
      lines.length = 0;
      return result;
    });
  }

  function send(command: string): void {
    instance.postMessage(command);
  }

  function dispose(): void {
    try {
      instance.terminate();
    } catch {
      /* ignore */
    }
  }

  return { send, solve, dispose };
}

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

const nowMs = (): number =>
  typeof performance !== "undefined" ? performance.now() : Date.now();

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
  errorMsg: string,
): Promise<void> {
  instance.postMessage(command);
  const ok = await waitFor(pred, timeoutMs);
  if (!ok) throw new Error(errorMsg);
}

function parseSolveResult(
  goLines: string[],
  wallMs: number,
): SolveResult {
  const checkmateLine = goLines.find((l) => l.startsWith("checkmate")) ?? "";
  const lastInfo = [...goLines].reverse().find((l) => l.startsWith("info ")) ??
    null;

  const tokens = checkmateLine.split(/\s+/).slice(1);
  const status: SolveStatus = (() => {
    if (tokens.length === 0) return "none";
    const head = tokens[0];
    if (head === "nomate") return "nomate";
    if (head === "timeout") return "timeout";
    if (head === "none") return "none";
    return "mate";
  })();

  const moves = status === "mate" ? tokens : [];

  const nodesMatch = lastInfo?.match(/\bnodes\s+(\d+)/);
  const timeMatch = lastInfo?.match(/\binfo time\s+(\d+)/) ??
    lastInfo?.match(/\btime\s+(\d+)/);

  return {
    status,
    moves,
    nodes: nodesMatch ? Number(nodesMatch[1]) : null,
    timeMs: timeMatch ? Number(timeMatch[1]) : null,
    wallMs,
    lastInfo,
  };
}
