/**
 * @ultemica/yaneuraou-mate-pthread
 *
 * YaneuraOu mate (tsume) solver for browsers, built with `EM_PTHREAD=1`.
 *
 * Requires `SharedArrayBuffer` and the COOP/COEP cross-origin isolation headers
 * (`Cross-Origin-Opener-Policy: same-origin` /
 *  `Cross-Origin-Embedder-Policy: require-corp`). Cloudflare Workers cannot
 * host this build — use `@ultemica/yaneuraou-mate-cfworkers` there.
 *
 * Compared to the single-thread cfworkers build:
 *   - `go mate <ms>` time limits are honored (pthread worker can be polled).
 *   - `checkmate timeout` is reachable from `SolveResult.status`.
 *   - The DfPn search itself is not parallelised; the extra thread is only
 *     used to keep the JS main thread responsive.
 *
 * ## Usage (browser, with COOP/COEP enabled)
 *
 * ```ts
 * import { createMateEngine } from "@ultemica/yaneuraou-mate-pthread";
 * import YaneuraOuFactory from "@ultemica/yaneuraou-mate-pthread/engine";
 * import wasmUrl from "@ultemica/yaneuraou-mate-pthread/wasm?url";
 *
 * const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
 * const engine = await createMateEngine({ factory: YaneuraOuFactory, wasmBinary });
 *
 * const result = await engine.solve({
 *   sfen: "...",
 *   byoyomi: 5_000,
 *   nodesLimit: 1_000_000,
 * });
 * ```
 */

// -------------------------------------------------------------------
// Types
// -------------------------------------------------------------------

export type YaneuraOuFactory = (opts?: {
  wasmBinary?: ArrayBuffer | Uint8Array;
  locateFile?: (path: string) => string;
  mainScriptUrlOrBlob?: string | Blob;
  print?: (line: string) => void;
  printErr?: (line: string) => void;
  [key: string]: unknown;
}) => Promise<YaneuraOuInstance>;

export interface YaneuraOuInstance {
  postMessage(command: string): void;
  addMessageListener(listener: (line: string) => void): void;
  removeMessageListener(listener: (line: string) => void): void;
  terminate(): void;
  HEAPU8: Uint8Array;
}

export interface CreateMateEngineOptions {
  factory: YaneuraOuFactory;
  wasmBinary: ArrayBuffer | Uint8Array;
  /**
   * If your bundler does not preserve `import.meta.url` for the engine `.js`,
   * pass the engine script URL (or a `Blob`) here so pthread workers can spawn.
   */
  mainScriptUrlOrBlob?: string | Blob;
  /** DfPn hash size in MB. Default `1`. */
  usiHash?: number;
  /** `Threads` USI option. Default `1` (DfPn is not parallelised). */
  threads?: number;
  /** `usi` handshake timeout in ms. Default `5_000`. */
  handshakeTimeoutMs?: number;
  /** `isready` timeout in ms. Default `10_000`. */
  readyTimeoutMs?: number;
}

export interface SolveRequest {
  /** SFEN string (the part after `position sfen`). */
  sfen: string;
  /**
   * Time limit in ms passed as `go mate <ms>`. `0` (default) means no time
   * limit; search runs until mate is found or `nodesLimit` is hit. In the
   * pthread build the time limit is honored and `status === "timeout"` is
   * possible.
   */
  byoyomi?: number;
  /** Node limit (0 = unlimited). Default `0`. */
  nodesLimit?: number;
}

export type SolveStatus = "mate" | "nomate" | "timeout" | "none";

export interface SolveResult {
  status: SolveStatus;
  moves: string[];
  nodes: number | null;
  timeMs: number | null;
  wallMs: number;
  lastInfo: string | null;
}

export interface MateEngine {
  send(command: string): void;
  solve(req: SolveRequest): Promise<SolveResult>;
  dispose(): void;
}

// -------------------------------------------------------------------
// Implementation
// -------------------------------------------------------------------

export async function createMateEngine(
  opts: CreateMateEngineOptions,
): Promise<MateEngine> {
  if (typeof SharedArrayBuffer === "undefined") {
    throw new Error(
      "yaneuraou-mate-pthread requires SharedArrayBuffer. Serve the page " +
        "with COOP=same-origin and COEP=require-corp headers.",
    );
  }

  const usiHash = opts.usiHash ?? 1;
  const threads = opts.threads ?? 1;
  const handshakeTimeout = opts.handshakeTimeoutMs ?? 5_000;
  const readyTimeout = opts.readyTimeoutMs ?? 10_000;

  const lines: string[] = [];
  const factoryArgs: Parameters<YaneuraOuFactory>[0] = {
    wasmBinary: opts.wasmBinary,
  };
  if (opts.mainScriptUrlOrBlob !== undefined) {
    factoryArgs.mainScriptUrlOrBlob = opts.mainScriptUrlOrBlob;
  }
  const instance = await opts.factory(factoryArgs);
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

  instance.postMessage(`setoption name Threads value ${threads}`);
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
