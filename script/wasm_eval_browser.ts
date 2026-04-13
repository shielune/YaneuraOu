#!/usr/bin/env bun
// Run a WASM-built YaneuraOu engine in headless Chromium via Playwright and
// print the eval as JSON (same shape as script/wasm_eval_test.mjs).
//
// Usage:
//   bun script/wasm_eval_browser.ts <path-to-yaneuraou.<pkg>.js> [--think-ms 30000]

import { chromium } from "playwright";
import { createServer } from "node:http";
import { readFileSync, existsSync, statSync } from "node:fs";
import { extname, dirname, basename, resolve, join } from "node:path";
import { fileURLToPath } from "node:url";

const SFEN = "lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1";
const DEFAULT_THINK_MS = 30000;

const argv = process.argv.slice(2);
if (argv.length < 1) {
  console.error("usage: bun script/wasm_eval_browser.ts <yaneuraou.<pkg>.js> [--think-ms N]");
  process.exit(2);
}
const jsPath = resolve(argv[0]!);
let thinkMs = DEFAULT_THINK_MS;
for (let i = 1; i < argv.length; i++) {
  if (argv[i] === "--think-ms") thinkMs = Number(argv[++i]);
}
if (!existsSync(jsPath)) {
  console.error(`not found: ${jsPath}`);
  process.exit(2);
}
const libDir = dirname(jsPath);
const engineFile = basename(jsPath);

const scriptDir = dirname(fileURLToPath(import.meta.url));
const runnerHtmlPath = join(scriptDir, "wasm_eval_runner.html");

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".mjs": "application/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".json": "application/json; charset=utf-8",
};

// Static server serving libDir at /engine/, runner.html at /runner.html.
// Sends COOP/COEP so crossOriginIsolated = true → SharedArrayBuffer enabled.
const server = createServer((req, res) => {
  const coiHeaders = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
  };
  try {
    const urlPath = (req.url ?? "/").split("?")[0]!;
    let filePath: string | null = null;
    if (urlPath === "/runner.html") {
      filePath = runnerHtmlPath;
    } else if (urlPath.startsWith("/engine/")) {
      const rel = urlPath.slice("/engine/".length);
      filePath = join(libDir, rel);
      if (!filePath.startsWith(libDir)) filePath = null;
    }
    if (!filePath || !existsSync(filePath) || !statSync(filePath).isFile()) {
      res.writeHead(404, coiHeaders);
      res.end("not found: " + urlPath);
      return;
    }
    const mime = MIME[extname(filePath)] ?? "application/octet-stream";
    let body: Buffer | string = readFileSync(filePath);
    // Newer emscripten (>= 3.1.60) only routes pthread stdout back to the
    // main thread when `Module.print.proxy === true`. wasm_pre.js doesn't
    // set it, so worker-side print falls back to `console.log` inside the
    // worker and we never see `info`/`bestmove`. Inject the flag after the
    // dispatcher is installed.
    if (mime.startsWith("application/javascript")) {
      let text = body.toString("utf8");
      // Newer emscripten (>= 3.1.74) ships an empty proxy handler list to
      // pthread workers, so YaneuraOu's wasm_pre.js `Module.print` dispatcher
      // runs inside the worker and falls back to the worker's own
      // `console.log` — stdout never reaches the main thread, and we never
      // see `info`/`bestmove`. Also the main-thread `sa = console.log.bind()`
      // is hard-coded, so Module.print is ignored there too.
      //
      // Workaround: inject a prelude that, when loaded inside a pthread
      // worker, reroutes `console.log` through postMessage. On the main
      // thread we patch `Worker.prototype.addEventListener` to intercept
      // those messages.
      const prelude =
        "try{if(typeof self!=='undefined'&&self.name==='em-pthread'){" +
        "self.postMessage({__yaneurao_stdout:true,text:'[worker-prelude-loaded]'});" +
        "var __el=console.log.bind(console);" +
        "console.log=function(){try{var s=Array.prototype.map.call(arguments,String).join(' ');" +
        "self.postMessage({__yaneurao_stdout:true,text:s});}catch(e){__el.apply(null,arguments);}};" +
        "console.error=console.log;}}catch(e){}\n";
      text = prelude + text;
      body = Buffer.from(text, "utf8");
    }
    res.writeHead(200, { ...coiHeaders, "Content-Type": mime, "Content-Length": String(body.length) });
    res.end(body);
  } catch (e) {
    res.writeHead(500, { "Content-Type": "text/plain" });
    res.end(String(e));
  }
});

await new Promise<void>((r) => server.listen(0, "127.0.0.1", () => r()));
const addr = server.address();
if (!addr || typeof addr === "string") throw new Error("server.address()");
const origin = `http://127.0.0.1:${addr.port}`;

const versionTag = basename(dirname(dirname(libDir))); // build/<ver>/<pkg>/lib -> <ver>
const engineTag = basename(dirname(libDir));            // -> <pkg>

let exitCode = 0;
const browser = await chromium.launch({ headless: true });
try {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  page.on("pageerror", (err) => console.error("[pageerror]", err.message));
  page.on("console", (msg) => {
    const t = msg.type();
    if (t === "error" || t === "warning" || t === "log") console.error(`[${t}]`, msg.text());
  });

  const params = new URLSearchParams({
    engine: `/engine/${engineFile}`,
    sfen: SFEN,
    think: String(thinkMs),
    threads: "1",
    hash: "64",
  });
  await page.goto(`${origin}/runner.html?${params}`);

  const result = await page.waitForFunction(
    () => (window as any).__result ?? ((window as any).__error ? { __error: (window as any).__error } : null),
    null,
    { timeout: thinkMs + 60000 },
  );
  const value = await result.jsonValue() as any;
  const pageLog = await page.evaluate(() => (document.getElementById("log") as HTMLElement).textContent ?? "");

  if (value && value.__error) {
    console.log(JSON.stringify({ version: versionTag, engine: engineTag, thinkMs, error: value.__error, log: pageLog }, null, 2));
    exitCode = 3;
  } else {
    console.log(JSON.stringify({
      version: versionTag,
      engine: engineTag,
      thinkMs,
      sfen: SFEN,
      score: value.score,
      bestmove: value.bestmove,
      lastInfo: value.lastInfo,
      infoCount: value.infoCount,
    }, null, 2));
  }
} catch (e) {
  console.log(JSON.stringify({ version: versionTag, engine: engineTag, thinkMs, error: String((e as Error)?.stack ?? e) }, null, 2));
  exitCode = 3;
} finally {
  await browser.close();
  server.close();
}
process.exit(exitCode);
