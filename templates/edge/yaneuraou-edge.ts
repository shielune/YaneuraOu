/**
 * YaneuraOu (WASM / edge variant) — 最小ラッパー
 *
 * V8 Isolate 系ランタイム (Cloudflare Workers / Vercel Edge Functions /
 * Deno Deploy / Cloudflare Pages Functions 等) で
 * `build/<ver>_<arch>/<pkg>/edge/lib/yaneuraou.<pkg>.js` を駆動するための
 * フレームワーク非依存ラッパー。
 *
 * このファイルはコピペして自分のプロジェクトに取り込んで使う想定で、
 * 外部依存は無い (`@types/emscripten` にも依存しない)。
 *
 * ## 使用例 (Cloudflare Workers / Vercel Edge 等)
 *
 * ```ts
 * import YaneuraOu_K_P from "./yaneuraou.k-p.js";
 * import wasmBinary from "./yaneuraou.k-p.wasm"; // bundler が ArrayBuffer で渡すよう設定
 * import { createYaneuraOuEdge } from "./yaneuraou-edge";
 *
 * // module scope でエンジンを 1 つ用意 (Isolate 寿命中は再利用)
 * const enginePromise = createYaneuraOuEdge({
 *   factory: YaneuraOu_K_P as unknown as YaneuraOuFactory,
 *   wasmBinary,
 * });
 *
 * export default {
 *   async fetch(req: Request): Promise<Response> {
 *     const { sfen, byoyomi = 500 } = await req.json<{
 *       sfen: string;
 *       byoyomi?: number;
 *     }>();
 *     const engine = await enginePromise;
 *     const result = await engine.eval({ sfen, byoyomi });
 *     return Response.json(result);
 *   },
 * };
 * ```
 *
 * ## 制約 (edge variant 共通)
 *
 * - `Threads` は 1 固定。`setoption name Threads value N` を送っても無効。
 * - `USI_Ponder` / `Stochastic_Ponder` は動かない。探索と別スレッドで
 *   USI ループを回せないため。
 * - 探索中に `stop` / `setoption` を送る手段は無い。`go` は呼び出し
 *   スレッドで同期実行される。
 * - `go btime/wtime/byoyomi` は `MinimumThinkingTime` 等に削られるので、
 *   このラッパーは内部で `go movetime` を発行する。
 * - 連続 `eval()` 呼び出しは内部で自動直列化される。
 */

// ---------------------------------------------------------------
// 最小限の型定義
// ---------------------------------------------------------------

/**
 * `yaneuraou.<pkg>.js` を `import` して得られるモジュール初期化関数。
 * emscripten の `EmscriptenModuleFactory` を緩めたもの。
 */
export type YaneuraOuFactory = (opts?: {
  wasmBinary?: ArrayBuffer | Uint8Array;
  locateFile?: (path: string) => string;
  print?: (line: string) => void;
  printErr?: (line: string) => void;
  [key: string]: unknown;
}) => Promise<YaneuraOuInstance>;

/**
 * factory を await した結果。`wasm_pre.js` が生やす USI ブリッジ API
 * と、必要最小限の emscripten runtime メソッドを含む。
 */
export interface YaneuraOuInstance {
  postMessage(command: string): void;
  addMessageListener(listener: (line: string) => void): void;
  removeMessageListener(listener: (line: string) => void): void;
  terminate(): void;
}

// ---------------------------------------------------------------
// 公開 API
// ---------------------------------------------------------------

export interface YaneuraOuEdgeOptions {
  /** `yaneuraou.<pkg>.js` から import した factory。 */
  factory: YaneuraOuFactory;
  /** `yaneuraou.<pkg>.wasm` のバイト列。bundler でバイナリとして import するか、fetch で取得する。 */
  wasmBinary: ArrayBuffer | Uint8Array;
  /** 置換表サイズ MB。default 16。 */
  usiHash?: number;
  /** Stockfish 互換 Hash option の MB。default は `usiHash` と同値。 */
  hash?: number;
  /** `usi` → `usiok` のタイムアウト ms。default 10_000。 */
  handshakeTimeoutMs?: number;
  /** `isready` → `readyok` のタイムアウト ms。default 30_000。 */
  readyTimeoutMs?: number;
}

export interface EvalRequest {
  /** SFEN 文字列 (`position sfen …` の後ろに渡す)。 */
  sfen: string;
  /** 思考時間 ms。default 500。内部では `go movetime` として発行する。 */
  byoyomi?: number;
  /**
   * SkillLevel (0–20)。default 20 = 手加減なし。
   * 省略すると毎回 20 (USI 既定値) で上書きされるので、前回の
   * eval() で立てた値を引きずらない。
   */
  skillLevel?: number;
  /** MultiPV (default 1)。省略時は 1 で毎回上書き。 */
  multiPv?: number;
  /** 探索深さ上限 (0 = 無制限、default 0)。省略時は 0 で毎回上書き。 */
  depthLimit?: number;
  /** 探索ノード数上限 (0 = 無制限、default 0)。省略時は 0 で毎回上書き。 */
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
  /** 最後の `info … score …` 行 (生文字列)。 */
  lastInfo: string | null;
  /** `bestmove …` 行 (生文字列)。 */
  bestmoveLine: string;
}

export interface YaneuraOuEdge {
  /**
   * 1 手分の思考 (iterative deepening search) を走らせ、bestmove と
   * 評価値を返す。複数同時に呼ぶと内部で自動的に直列化されるので、
   * 呼び出し側でロックを取る必要は無い。
   *
   * JS グローバルの `eval` と名前は被るが、オブジェクトメソッドなので
   * 動作上の衝突は無い (linter の `no-eval` ルールが誤検知する場合は
   * `engine["eval"](…)` で呼ぶか、ローカル変数にエイリアスする)。
   */
  eval(req: EvalRequest): Promise<EvalResult>;
  /** Isolate 解放前に呼ぶ。呼び忘れても致命的ではない。 */
  dispose(): void;
}

// ---------------------------------------------------------------
// 実装
// ---------------------------------------------------------------

export async function createYaneuraOuEdge(
  opts: YaneuraOuEdgeOptions,
): Promise<YaneuraOuEdge> {
  const usiHash = opts.usiHash ?? 16;
  const hash = opts.hash ?? usiHash;
  const handshakeTimeout = opts.handshakeTimeoutMs ?? 10_000;
  const readyTimeout = opts.readyTimeoutMs ?? 30_000;

  const lines: string[] = [];
  const instance = await opts.factory({ wasmBinary: opts.wasmBinary });
  instance.addMessageListener((line) => {
    lines.push(line);
  });

  // 初期 handshake。
  await sendAndWait(
    instance,
    "usi",
    () => lines.includes("usiok"),
    handshakeTimeout,
    "usi → usiok timeout",
  );

  // Threads / Hash を設定し、初回の isready で NNUE の重み展開 + 置換表確保を済ませる。
  instance.postMessage("setoption name Threads value 1");
  instance.postMessage(`setoption name USI_Hash value ${usiHash}`);
  instance.postMessage(`setoption name Hash value ${hash}`);
  const initMark = lines.length;
  await sendAndWait(
    instance,
    "isready",
    () => lines.slice(initMark).includes("readyok"),
    readyTimeout,
    "initial isready → readyok timeout",
  );

  // 直列化用の promise chain。
  let chain: Promise<unknown> = Promise.resolve();
  const serial = <T>(fn: () => Promise<T>): Promise<T> => {
    const next = chain.then(fn, fn);
    chain = next.catch(() => undefined);
    return next;
  };

  async function evalPosition(req: EvalRequest): Promise<EvalResult> {
    return serial(async () => {
      // request-level option は毎回明示的に再設定する。
      // 省略された場合も USI 既定値で上書きしてリセットするので、
      // 同じ Isolate に届いた前の eval() の値を引きずらない
      // (edge 環境では複数ユーザーが同じ instance を共有する
      // 前提になるため)。
      instance.postMessage(
        `setoption name MultiPV value ${req.multiPv ?? 1}`,
      );
      instance.postMessage(
        `setoption name SkillLevel value ${req.skillLevel ?? 20}`,
      );
      instance.postMessage(
        `setoption name DepthLimit value ${req.depthLimit ?? 0}`,
      );
      instance.postMessage(
        `setoption name NodesLimit value ${req.nodesLimit ?? 0}`,
      );

      instance.postMessage("usinewgame");
      const readyMark = lines.length;
      await sendAndWait(
        instance,
        "isready",
        () => lines.slice(readyMark).includes("readyok"),
        readyTimeout,
        "eval isready → readyok timeout",
      );

      instance.postMessage(`position sfen ${req.sfen}`);
      const movetime = req.byoyomi ?? 500;
      const goMark = lines.length;
      instance.postMessage(`go movetime ${movetime}`);

      const ok = await waitFor(
        () =>
          lines.slice(goMark).some((l) => l.startsWith("bestmove")),
        movetime + 30_000,
      );
      if (!ok) throw new Error(`go movetime ${movetime} → bestmove timeout`);

      const goLines = lines.slice(goMark);

      // 過剰なメモリ消費を避けるため、この go 分の行はここで破棄する。
      // 以降の eval() は lines[0..] を見れば十分なので、単に切り詰める。
      lines.length = 0;

      return parseGoResult(goLines);
    });
  }

  function dispose(): void {
    try {
      instance.terminate();
    } catch {
      /* ignore */
    }
  }

  return { eval: evalPosition, dispose };
}

// ---------------------------------------------------------------
// ヘルパー
// ---------------------------------------------------------------

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
