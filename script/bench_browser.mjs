#!/usr/bin/env node

/**
 * ブラウザ (headless Chromium) での固定ノードベンチマーク
 *
 * script/bench_nodes.mjs の node 版と同じ条件で、EM_ENVIRONMENT=web,worker で
 * ビルドした変種を実ブラウザ上で走らせて比較するためのもの。
 *
 * pthread 版の WASM は SharedArrayBuffer を要求するので、配信側に
 * COOP/COEP ヘッダが要る。そのための静的サーバーも内包している。
 *
 * Usage:
 *   node script/bench_browser.mjs --dir <yaneuraou.js のあるディレクトリ> \
 *        --eval assets/eval/k_p_256/suisho/nn.bin [--nodes N] [--hash MB] [--threads N]
 *
 *   --go "depth 12" でノード数指定の代わりに深さ指定にできる。
 *   --chromium <path> で実行ファイルを指定 (既定は playwright のキャッシュを探す)
 */

import { spawn } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, resolve } from "node:path";

// ---------------------------------------------------------------- args

const argv = process.argv.slice(2);
const opt = {
	dir: null,
	eval: null,
	nodes: 1_000_000,
	hash: 256,
	threads: 1,
	go: null,
	sfen: "sfen l1g1k2nl/1r4g2/2nsppsp1/p1pp2p1p/1p4PP1/P1P2P2P/1PSPPS3/2GK1G1R1/LN5NL w Bb 32",
	chromium: null,
};
for (let i = 0; i < argv.length; i++) {
	const next = () => argv[++i];
	const a = argv[i];
	if (a === "--dir") opt.dir = resolve(next());
	else if (a === "--eval") opt.eval = resolve(next());
	else if (a === "--nodes") opt.nodes = Number(next());
	else if (a === "--hash") opt.hash = Number(next());
	else if (a === "--threads") opt.threads = Number(next());
	else if (a === "--go") opt.go = next();
	else if (a === "--sfen") opt.sfen = next();
	else if (a === "--chromium") opt.chromium = resolve(next());
	else throw new Error(`unknown option: ${a}`);
}
if (!opt.dir) throw new Error("--dir is required");

async function findChromium() {
	if (opt.chromium) return opt.chromium;
	// playwright が入れたものを使う。
	const { chromium } = await import("playwright");
	const p = chromium.executablePath();
	if (!p || !existsSync(p))
		throw new Error("chromium not found; pass --chromium <path>");
	return p;
}

// ---------------------------------------------------------------- server

const MIME = {
	".html": "text/html",
	".js": "application/javascript",
	".mjs": "application/javascript",
	".wasm": "application/wasm",
	".bin": "application/octet-stream",
};

// エンジンを駆動するページ。結果を window.__done に置く。
const PAGE = `<!doctype html><meta charset="utf-8"><title>bench</title><script type="module">
const lines = [];
window.__lines = lines;
const factory = (await import("./yaneuraou.js")).default;
const engine = await factory({});
engine.addMessageListener((l) => lines.push(l));
const waitFor = (re, ms) => new Promise((res, rej) => {
  const t0 = Date.now();
  const iv = setInterval(() => {
    const hit = lines.find((l) => re.test(l));
    if (hit) { clearInterval(iv); res(hit); }
    else if (Date.now() - t0 > ms) { clearInterval(iv); rej(new Error("timeout " + re)); }
  }, 50);
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
try {
  engine.postMessage("usi");
  await waitFor(/usiok/, 30000);
  const evalBin = await fetch("./nn.bin").then((r) => r.arrayBuffer());
  engine.FS.mkdir("/eval");
  engine.FS.writeFile("/eval/nn.bin", new Uint8Array(evalBin));
  for (const c of window.__cmds) {
    engine.postMessage(c);
    if (c.startsWith("setoption")) await sleep(300);
    if (c === "isready") await waitFor(/readyok/, 300000);
  }
  await waitFor(/^bestmove/, 900000);
  window.__done = { ok: true, lines };
} catch (e) {
  window.__done = { ok: false, error: String(e), lines };
}
</script>`;

function startServer(dir, evalPath) {
	return new Promise((ok) => {
		const srv = createServer((req, res) => {
			const url = new URL(req.url, `http://${req.headers.host}`);
			// pthread ビルドは SharedArrayBuffer を使うので cross-origin isolation が要る。
			const headers = {
				"Cross-Origin-Opener-Policy": "same-origin",
				"Cross-Origin-Embedder-Policy": "require-corp",
			};
			if (url.pathname === "/" || url.pathname === "/index.html") {
				res.writeHead(200, { ...headers, "Content-Type": "text/html" });
				return res.end(PAGE);
			}
			const file =
				url.pathname === "/nn.bin"
					? evalPath
					: join(dir, url.pathname.slice(1));
			if (!existsSync(file)) {
				res.writeHead(404);
				return res.end("not found");
			}
			res.writeHead(200, {
				...headers,
				"Content-Type": MIME[extname(file)] ?? "application/octet-stream",
			});
			res.end(readFileSync(file));
		});
		srv.listen(0, "127.0.0.1", () => ok({ srv, port: srv.address().port }));
	});
}

// ---------------------------------------------------------------- main

const { srv, port } = await startServer(opt.dir, opt.eval);
const cmds = [
	`setoption name USI_Hash value ${opt.hash}`,
	`setoption name Threads value ${opt.threads}`,
	"setoption name USI_OwnBook value false",
	"setoption name PvInterval value 0",
	"setoption name EvalDir value /eval",
	"isready",
	"usinewgame",
	`position ${opt.sfen}`,
	`go ${opt.go ?? `nodes ${opt.nodes}`}`,
];

const { chromium } = await import("playwright");
const browser = await chromium.launch({
	executablePath: await findChromium(),
	args: ["--no-sandbox", "--enable-features=SharedArrayBuffer"],
});
const page = await browser.newPage();
page.on("pageerror", (e) => console.error("[pageerror]", e.message));
await page.addInitScript((c) => {
	window.__cmds = c;
}, cmds);
await page.goto(`http://127.0.0.1:${port}/`);

const result = await page.waitForFunction(() => window.__done, null, {
	timeout: 900_000,
});
const done = await result.jsonValue();
await browser.close();
srv.close();

if (!done.ok) {
	console.error("FAILED:", done.error);
	console.error((done.lines ?? []).slice(-5).join("\n"));
	process.exit(1);
}
const info = done.lines.filter((l) => l.startsWith("info depth")).pop() ?? "";
const best = done.lines.find((l) => l.startsWith("bestmove")) ?? "";
console.log(info);
console.log(best);
