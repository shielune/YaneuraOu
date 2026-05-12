/**
 * End-to-end smoke test for yaneuraou-wasm-mate-pthread.
 *
 * Starts a static server with COOP/COEP headers, opens it in headless
 * Chromium via Playwright, and runs `runMateTest()` against a 3-ply mate
 * SFEN. Exits non-zero on any failure.
 *
 * `bun run test/run.ts`
 */

import { spawn } from "node:child_process";
import { chromium } from "playwright";

const MATE3_SFEN =
  "ln1gkg1nl/6+P2/2sppps1p/2p3p2/p8/P1P1P3P/2NP1PP2/3s1KSR1/L1+b2G1NL w R2Pbgp 42";

function startServer(): Promise<{ url: string; kill: () => void }> {
  return new Promise((resolveStart, rejectStart) => {
    const proc = spawn(
      "bun",
      ["run", new URL("./server.ts", import.meta.url).pathname],
      { stdio: ["ignore", "pipe", "inherit"] },
    );
    const kill = () => proc.kill("SIGTERM");
    proc.on("error", rejectStart);
    proc.stdout.on("data", (chunk: Buffer) => {
      const url = chunk.toString().trim().split("\n")[0];
      if (url.startsWith("http://")) resolveStart({ url, kill });
    });
    setTimeout(() => rejectStart(new Error("server start timeout")), 5_000);
  });
}

const main = async () => {
  const { url, kill } = await startServer();
  console.log(`server: ${url}`);

  const browser = await chromium.launch({
    headless: true,
    args: ["--enable-features=SharedArrayBuffer"],
  });
  const context = await browser.newContext();
  const page = await context.newPage();
  page.on("console", (m) => console.log(`[browser ${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => console.error(`[browser error]`, e.message));

  await page.goto(url, { waitUntil: "load" });
  await page.waitForFunction(
    () => typeof (window as unknown as { runMateTest?: unknown }).runMateTest ===
      "function",
    null,
    { timeout: 30_000 },
  );

  const result = await page.evaluate(
    async (sfen) =>
      await (window as unknown as { runMateTest: (s: string, o?: object) => Promise<unknown> }).runMateTest(sfen, { byoyomi: 5_000, nodesLimit: 100_000 }),
    MATE3_SFEN,
  ) as {
    status: string;
    moves: string[];
    nodes: number | null;
    timeMs: number | null;
    wallMs: number;
    lastInfo: string | null;
  };

  console.log(`result:`, JSON.stringify(result, null, 2));

  await browser.close();
  kill();

  if (result.status !== "mate") {
    console.error(`expected status=mate, got ${result.status}`);
    process.exit(1);
  }
  if (result.moves.length !== 3) {
    console.error(`expected 3 moves, got ${result.moves.length}: ${result.moves.join(" ")}`);
    process.exit(1);
  }
  console.log(`OK: ${result.moves.join(" ")} (nodes=${result.nodes}, wall=${result.wallMs}ms)`);
};

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
