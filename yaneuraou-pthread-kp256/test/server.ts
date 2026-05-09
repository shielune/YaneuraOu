/**
 * Static server for the Playwright test. Serves files from the package root
 * with the COOP/COEP cross-origin isolation headers required by pthread WASM.
 *
 * `bun run test/server.ts [port]`
 */

import { existsSync, readFileSync, statSync } from "node:fs";
import { join, resolve } from "node:path";

const PORT = Number(process.argv[2] ?? 0);
const ROOT = resolve(import.meta.dir, "..");

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".mjs": "application/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".wasm": "application/wasm",
};

const ISOLATION_HEADERS = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

const server = Bun.serve({
  port: PORT,
  fetch(req) {
    const url = new URL(req.url);
    const path = url.pathname === "/" ? "/test/index.html" : url.pathname;
    // Strip leading slash and resolve against ROOT, then assert the result
    // stays within ROOT — `path.join(ROOT, "/../etc/passwd")` normalises to
    // `/etc/passwd`, so without this check a crafted request could read
    // arbitrary host files.
    const filePath = resolve(join(ROOT, path.replace(/^\/+/, "")));
    if (filePath !== ROOT && !filePath.startsWith(`${ROOT}/`)) {
      return new Response("forbidden", { status: 403 });
    }
    if (!existsSync(filePath) || !statSync(filePath).isFile()) {
      return new Response(`not found: ${path}`, { status: 404 });
    }
    const ext = filePath.slice(filePath.lastIndexOf("."));
    const headers = new Headers(ISOLATION_HEADERS);
    if (MIME[ext]) headers.set("Content-Type", MIME[ext]);
    return new Response(readFileSync(filePath), { headers });
  },
});

console.log(`http://localhost:${server.port}/`);
