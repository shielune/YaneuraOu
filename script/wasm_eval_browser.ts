#!/usr/bin/env bun
// Run a WASM-built YaneuraOu engine in headless Chromium via Playwright and
// print the eval as JSON (same shape as script/wasm_eval_node.ts).
//
// Usage:
//   bun script/wasm_eval_browser.ts <path-to-yaneuraou.<pkg>.js> [--think-ms 30000]
//
// The static server mounts:
//   /engine/*             -> libDir (the build's own directory)
//   /loaders/* , /script/* -> script/loaders/* and script/wasm_eval_common.ts
//                              (TypeScript is transpiled on the fly via Bun)
//   /runner.html          -> script/wasm_eval_runner.html
//
// It also injects a small prelude into the engine JS so that pthread-side
// `console.log` is rerouted through `postMessage({__yaneurao_stdout, …})`,
// which the ccall-only browser loader listens for. This cannot live in the
// loader itself because we need to intercept *before* the engine worker
// runs, and workers are spawned from inside the generated JS.

import { chromium } from "playwright";
import { createServer } from "node:http";
import { readFileSync, existsSync, statSync } from "node:fs";
import { extname, dirname, basename, resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import { detectVersionFromJsPath } from "./loaders/detect.ts";

const DEFAULT_SFEN =
  "lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1";
const DEFAULT_THINK_MS = 30000;

const argv = process.argv.slice(2);
if (argv.length < 1) {
  console.error(
    "usage: bun script/wasm_eval_browser.ts <yaneuraou.<pkg>.js> [--think-ms N] [--sfen '<sfen>'] [--expect-version <regex>]",
  );
  process.exit(2);
}
const jsPath = resolve(argv[0]!);
let thinkMs = DEFAULT_THINK_MS;
let SFEN = DEFAULT_SFEN;
let expectVersion: RegExp | null = null;
for (let i = 1; i < argv.length; i++) {
  if (argv[i] === "--think-ms") thinkMs = Number(argv[++i]);
  else if (argv[i] === "--sfen") SFEN = String(argv[++i]);
  else if (argv[i] === "--expect-version") expectVersion = new RegExp(String(argv[++i]));
}
if (!existsSync(jsPath)) {
  console.error(`not found: ${jsPath}`);
  process.exit(2);
}
const libDir = dirname(jsPath);
const engineFile = basename(jsPath);
const emscriptenVersion = detectVersionFromJsPath(jsPath);

const scriptDir = dirname(fileURLToPath(import.meta.url));
const runnerHtmlPath = join(scriptDir, "wasm_eval_runner.html");
const loadersDir = join(scriptDir, "loaders");
const commonFile = join(scriptDir, "wasm_eval_common.ts");

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".mjs": "application/javascript; charset=utf-8",
  ".ts": "application/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".json": "application/json; charset=utf-8",
};

const tsTranspiler = new Bun.Transpiler({ loader: "ts", target: "browser" });

function transpileTs(source: string): string {
  return tsTranspiler.transformSync(source);
}

const coiHeaders = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

// The pthread stdout tap prelude. This has to be injected server-side
// because it runs inside each pthread worker before the engine code does,
// so there is no other hook point. The matching main-thread Worker patch
// lives in script/loaders/browser/common.ts.
const PTHREAD_STDOUT_PRELUDE =
  "try{if(typeof self!=='undefined'&&self.name==='em-pthread'){" +
  "self.postMessage({__yaneurao_stdout:true,text:'[worker-prelude-loaded]'});" +
  "var __el=console.log.bind(console);" +
  "console.log=function(){try{var s=Array.prototype.map.call(arguments,String).join(' ');" +
  "self.postMessage({__yaneurao_stdout:true,text:s});}catch(e){__el.apply(null,arguments);}};" +
  "console.error=console.log;}}catch(e){}\n";

const server = createServer((req, res) => {
  try {
    const urlPath = (req.url ?? "/").split("?")[0]!;
    let filePath: string | null = null;
    let isEngineJs = false;

    if (urlPath === "/runner.html") {
      filePath = runnerHtmlPath;
    } else if (urlPath === "/script/wasm_eval_common.ts") {
      filePath = commonFile;
    } else if (urlPath.startsWith("/loaders/")) {
      const rel = urlPath.slice("/loaders/".length);
      filePath = join(loadersDir, rel);
      if (!filePath.startsWith(loadersDir)) filePath = null;
    } else if (urlPath.startsWith("/engine/")) {
      const rel = urlPath.slice("/engine/".length);
      filePath = join(libDir, rel);
      if (!filePath.startsWith(libDir)) filePath = null;
      isEngineJs = rel === engineFile;
    }

    if (!filePath || !existsSync(filePath) || !statSync(filePath).isFile()) {
      res.writeHead(404, coiHeaders);
      res.end("not found: " + urlPath);
      return;
    }

    const mime = MIME[extname(filePath)] ?? "application/octet-stream";
    let body: Buffer | string = readFileSync(filePath);

    if (extname(filePath) === ".ts") {
      body = Buffer.from(transpileTs(body.toString("utf8")), "utf8");
    } else if (isEngineJs && mime.startsWith("application/javascript")) {
      // Prepend the pthread stdout tap prelude to the engine JS only.
      const text = PTHREAD_STDOUT_PRELUDE + body.toString("utf8");
      body = Buffer.from(text, "utf8");
    }

    res.writeHead(200, {
      ...coiHeaders,
      "Content-Type": mime,
      "Content-Length": String(body.length),
    });
    res.end(body);
  } catch (e) {
    res.writeHead(500, { "Content-Type": "text/plain" });
    res.end(String(e));
  }
});

await new Promise<void>((r) =>
  server.listen(0, "127.0.0.1", () => r()),
);
const addr = server.address();
if (!addr || typeof addr === "string") throw new Error("server.address()");
const origin = `http://127.0.0.1:${addr.port}`;

// New layout: build/<ver>_<arch>/<pkg>/<variant>/lib/
//   dirname(libDir):                    <variant>
//   dirname(dirname(libDir)):           <pkg>
//   dirname(dirname(dirname(libDir))):  <ver>_<arch>
const versionTag = basename(dirname(dirname(dirname(libDir))));
const engineTag = basename(dirname(dirname(libDir)));

let exitCode = 0;
const browser = await chromium.launch({ headless: true });
try {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  page.on("pageerror", (err) => console.error("[pageerror]", err.message));
  page.on("console", (msg) => {
    const t = msg.type();
    if (t === "error" || t === "warning" || t === "log") {
      console.error(`[${t}]`, msg.text());
    }
  });

  const params = new URLSearchParams({
    engine: `/engine/${engineFile}`,
    sfen: SFEN,
    think: String(thinkMs),
    threads: "1",
    hash: "64",
    version: emscriptenVersion,
  });
  await page.goto(`${origin}/runner.html?${params}`);

  const result = await page.waitForFunction(
    () =>
      (window as unknown as { __result?: unknown; __error?: unknown })
        .__result ??
      ((window as unknown as { __error?: unknown }).__error
        ? {
            __error: (window as unknown as { __error: unknown }).__error,
          }
        : null),
    null,
    { timeout: thinkMs + 60000 },
  );
  const value = (await result.jsonValue()) as {
    __error?: string;
    score?: unknown;
    bestmove?: unknown;
    lastInfo?: unknown;
    infoCount?: unknown;
  };
  const pageLog = await page.evaluate(
    () =>
      (document.getElementById("log") as HTMLElement | null)?.textContent ?? "",
  );

  if (value && value.__error) {
    console.log(
      JSON.stringify(
        {
          runner: "browser",
          version: versionTag,
          engine: engineTag,
          thinkMs,
          error: value.__error,
          log: pageLog,
        },
        null,
        2,
      ),
    );
    exitCode = 3;
  } else {
    const v = value as unknown as {
      score?: unknown;
      bestmove?: unknown;
      lastInfo?: unknown;
      infoCount?: unknown;
      idName?: string | null;
      engineVersion?: string | null;
    };
    let engineVersionMismatch: string | null = null;
    if (
      expectVersion &&
      !(v.engineVersion && expectVersion.test(v.engineVersion))
    ) {
      engineVersionMismatch = `expected /${expectVersion.source}/, got '${v.engineVersion ?? "null"}' (id name: ${v.idName ?? "null"})`;
    }
    console.log(
      JSON.stringify(
        {
          runner: "browser",
          version: versionTag,
          engine: engineTag,
          thinkMs,
          sfen: SFEN,
          idName: v.idName ?? null,
          engineVersion: v.engineVersion ?? null,
          score: v.score,
          bestmove: v.bestmove,
          lastInfo: v.lastInfo,
          infoCount: v.infoCount,
          ...(engineVersionMismatch ? { engineVersionMismatch } : {}),
        },
        null,
        2,
      ),
    );
    if (engineVersionMismatch) {
      process.stderr.write(`[wasm_eval_browser] engine version mismatch: ${engineVersionMismatch}\n`);
      exitCode = 4;
    }
  }
} catch (e) {
  console.log(
    JSON.stringify(
      {
        runner: "browser",
        version: versionTag,
        engine: engineTag,
        thinkMs,
        error: String((e as Error)?.stack ?? e),
      },
      null,
      2,
    ),
  );
  exitCode = 3;
} finally {
  await browser.close();
  server.close();
}
process.exit(exitCode);
