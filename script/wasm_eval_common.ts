// Environment-agnostic USI search driver. Takes any EngineInstance that
// implements sendCommand + onLine and runs the standard flow:
//
//   usi -> setoption Threads/USI_Hash -> isready -> position sfen -> go
//
// Produces the same result shape as the legacy wasm_eval_test.mjs output,
// so both runners (wasm_eval_node.ts, wasm_eval_browser.ts) can feed into
// the same comparison/reporting pipeline.

import type { EngineInstance } from "./loaders/types.ts";

export interface EvalConfig {
  sfen: string;
  thinkMs: number;
  threads: number;
  hash: number;
}

export interface EvalResult {
  sfen: string;
  thinkMs: number;
  threads: number;
  hash: number;
  score: { kind: "cp" | "mate"; value: number } | null;
  bestmove: string | null;
  lastInfo: string | null;
  infoCount: number;
}

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

async function waitFor(
  pred: () => boolean,
  timeout: number,
): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < timeout) {
    if (pred()) return true;
    await sleep(20);
  }
  return false;
}

export async function runUsiEval(
  engine: EngineInstance,
  config: EvalConfig,
): Promise<EvalResult> {
  const lines: string[] = [];
  engine.onLine((line) => lines.push(line));

  await engine.sendCommand("usi");
  if (!(await waitFor(() => lines.includes("usiok"), 20000))) {
    throw new Error(
      "usi timeout — tail: " + JSON.stringify(lines.slice(-10)),
    );
  }

  await engine.sendCommand(`setoption name Threads value ${config.threads}`);
  await engine.sendCommand(`setoption name USI_Hash value ${config.hash}`);
  await engine.sendCommand("isready");
  if (!(await waitFor(() => lines.includes("readyok"), 60000))) {
    throw new Error(
      "isready timeout — tail: " + JSON.stringify(lines.slice(-10)),
    );
  }

  await engine.sendCommand(`position sfen ${config.sfen}`);
  const goStart = lines.length;
  await engine.sendCommand(`go btime 0 wtime 0 byoyomi ${config.thinkMs}`);

  if (
    !(await waitFor(
      () => lines.slice(goStart).some((l) => l.startsWith("bestmove")),
      config.thinkMs + 15000,
    ))
  ) {
    throw new Error(
      "go timeout — tail: " +
        JSON.stringify(lines.slice(goStart).slice(-10)),
    );
  }

  const goLines = lines.slice(goStart);
  const infos = goLines.filter(
    (l) => l.startsWith("info") && l.includes(" score "),
  );
  const lastInfo = infos[infos.length - 1] ?? null;
  const bestmove = goLines.find((l) => l.startsWith("bestmove")) ?? null;
  const m = lastInfo && lastInfo.match(/\bscore (cp|mate) (-?\d+)/);
  const score = m
    ? { kind: m[1] as "cp" | "mate", value: Number(m[2]) }
    : null;

  return {
    sfen: config.sfen,
    thinkMs: config.thinkMs,
    threads: config.threads,
    hash: config.hash,
    score,
    bestmove,
    lastInfo,
    infoCount: infos.length,
  };
}
