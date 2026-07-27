/**
 * Smoke test for yaneuraou-wasm-pthread-kp256.
 *
 * Starts a static server with COOP/COEP headers, opens it in headless
 * Chromium via Playwright, and verifies that the WASM module instantiates
 * and completes the `usi` handshake under cross-origin isolation. Eval and
 * book files are not exercised here — that requires `nn.bin` and is the
 * responsibility of downstream applications.
 *
 * `bun run test/run.ts`
 */

import { spawn } from "node:child_process";
import { chromium } from "playwright";

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

  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext();
  const page = await context.newPage();
  page.on("console", (m) => console.log(`[browser ${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => console.error(`[browser error]`, e.message));

  await page.goto(url, { waitUntil: "load" });
  await page.waitForFunction(
    () =>
      typeof (window as unknown as { runSmokeTest?: unknown }).runSmokeTest ===
        "function",
    null,
    { timeout: 30_000 },
  );

  const result = (await page.evaluate(async () =>
    await (
      window as unknown as { runSmokeTest: () => Promise<unknown> }
    ).runSmokeTest()
  )) as {
    ok: boolean;
    idName: string | null;
    optionCount: number;
    totalLines: number;
  };

  console.log(`result:`, JSON.stringify(result, null, 2));

  await browser.close();
  kill();

  if (!result.ok) {
    console.error("smoke test reported not ok");
    process.exit(1);
  }
  if (!result.idName?.includes("YaneuraOu")) {
    console.error(`expected id name to mention YaneuraOu, got: ${result.idName}`);
    process.exit(1);
  }
  if (result.optionCount === 0) {
    console.error("expected option lines from usi handshake, got 0");
    process.exit(1);
  }
  console.log(
    `OK: ${result.idName} | ${result.optionCount} options | ${result.totalLines} total lines`,
  );
};

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
