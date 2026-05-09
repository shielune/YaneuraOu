/**
 * @ultemica/yaneuraou-pthread-kp256
 *
 * YaneuraOu NNUE KP256 shogi engine for browsers, built with `EM_PTHREAD=1`.
 *
 * Sister package to `@ultemica/yaneuraou-cfworkers`. Same `createEngine`
 * surface, but the WASM module is multi-threaded — search can use multiple
 * cores via the `Threads` USI option, at the cost of requiring
 * `SharedArrayBuffer` and the COOP/COEP cross-origin isolation headers.
 *
 * Cloudflare Workers cannot host this build — use
 * `@ultemica/yaneuraou-cfworkers` there.
 *
 * Eval (NNUE nn.bin) and book files are NOT embedded in the Wasm binary.
 * Pass them as ArrayBuffer at runtime via `createEngine()`. The loader
 * writes them into Emscripten's MEMFS so the C++ engine opens them
 * transparently through the standard `EvalDir` / `BookDir` USI options.
 *
 * ## Usage (browser, with COOP/COEP enabled)
 *
 * ```ts
 * import { createEngine } from "@ultemica/yaneuraou-pthread-kp256";
 * import YaneuraOuFactory from "@ultemica/yaneuraou-pthread-kp256/engine";
 * import wasmUrl from "@ultemica/yaneuraou-pthread-kp256/wasm?url";
 *
 * const wasmBinary = await fetch(wasmUrl).then((r) => r.arrayBuffer());
 * const evalBin = await fetch("/eval/nn.bin").then((r) => r.arrayBuffer());
 *
 * const engine = await createEngine({
 *   factory: YaneuraOuFactory,
 *   wasmBinary,
 *   evalBin,
 *   threads: navigator.hardwareConcurrency ?? 4,
 *   usiHash: 64,
 * });
 *
 * const result = await engine.eval({ sfen: "...", byoyomi: 1000 });
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
  FS: {
    mkdirTree(path: string): void;
    writeFile(path: string, data: Uint8Array): void;
  };
}

export interface CreateEngineOptions {
  factory: YaneuraOuFactory;
  wasmBinary: ArrayBuffer | Uint8Array;
  /**
   * If your bundler does not preserve `import.meta.url` for the engine `.js`,
   * pass the engine script URL (or a `Blob`) here so pthread workers can spawn.
   */
  mainScriptUrlOrBlob?: string | Blob;
  /** NNUE eval file contents. */
  evalBin?: ArrayBuffer;
  /** Eval file name written to MEMFS. Default `"nn.bin"`. */
  evalFile?: string;
  /** Opening book contents (optional). */
  bookDb?: ArrayBuffer;
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
// Implementation
// -------------------------------------------------------------------

export async function createEngine(opts: CreateEngineOptions): Promise<Engine> {
  if (typeof SharedArrayBuffer === "undefined") {
    throw new Error(
      "yaneuraou-pthread-kp256 requires SharedArrayBuffer. Serve the page " +
        "with COOP=same-origin and COEP=require-corp headers.",
    );
  }

  const usiHash = opts.usiHash ?? 64;
  const threads = opts.threads ?? 1;
  const handshakeTimeout = opts.handshakeTimeoutMs ?? 10_000;
  const readyTimeout = opts.readyTimeoutMs ?? 30_000;

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

  if (opts.evalBin) {
    const evalFile = opts.evalFile ?? "nn.bin";
    instance.FS.mkdirTree("/eval");
    instance.FS.writeFile(`/eval/${evalFile}`, new Uint8Array(opts.evalBin));
  }
  if (opts.bookDb && opts.bookFile) {
    instance.FS.mkdirTree("/book");
    instance.FS.writeFile(
      `/book/${opts.bookFile}`,
      new Uint8Array(opts.bookDb),
    );
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
