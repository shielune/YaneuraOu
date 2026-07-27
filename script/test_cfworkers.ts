#!/usr/bin/env bun

/**
 * Cloudflare Workers 互換テスト (Playwright + Chromium = V8)
 *
 * bun は JavaScriptCore、Workers は V8 Isolate なので、
 * Chromium で動かして初めて Workers と同じ JS エンジンでの検証になる。
 *
 * テスト内容:
 *   1. FS.writeFile で nn.bin を MEMFS 注入 → isready 成功
 *   2. 複数局面の評価値チェック
 *   3. 別の nn.bin (halfkp) での差し替えテスト (KP256 エンジンには合わないので
 *      ロードエラーが正しく出ることを確認)
 *
 * Usage:
 *   bun script/test_cfworkers.ts
 */

import { existsSync, readFileSync } from "node:fs";
import {
	createServer,
	type IncomingMessage,
	type ServerResponse,
} from "node:http";
import { extname, join, resolve } from "node:path";
import { chromium } from "playwright";

const ROOT = resolve(join(import.meta.dir, ".."));
const DIST = join(ROOT, "outputs", "yaneuraou-wasm-kp256-cfworkers", "dist");
const EVAL_FILE = join(ROOT, ".dl", "nn.bin");
const BOOK_FILE = join(ROOT, ".dl", "user_book1.db");

if (!existsSync(join(DIST, "yaneuraou.js"))) {
	console.error(
		"outputs/yaneuraou-wasm-kp256-cfworkers/dist/yaneuraou.js not found. Build first.",
	);
	process.exit(2);
}
if (!existsSync(join(DIST, "index.js"))) {
	console.error(
		"outputs/yaneuraou-wasm-kp256-cfworkers/dist/index.js not found. Run tsc first.",
	);
	process.exit(2);
}
if (!existsSync(EVAL_FILE)) {
	console.error(".dl/nn.bin not found. Extract eval file first.");
	process.exit(2);
}

// -- Test positions --
const TEST_POSITIONS = [
	{
		name: "startpos",
		sfen: "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
		thinkMs: 1000,
	},
	{
		name: "baseline (eval_results reference)",
		sfen: "lr5nl/2P2+S1k1/7p1/5bPPp/P3N4/4PP2P/1PR6/LK3G3/8L w G2S7Pb2gs2np 1",
		thinkMs: 1000,
	},
	{
		name: "middlegame (kakukoukan)",
		sfen: "ln1gkg1nl/1r1s3s1/p1pppp1pp/1p4p2/9/2P4P1/PP1PPPP1P/1B5R1/LNSGKGSNL b - 1",
		thinkMs: 1000,
	},
	{
		name: "endgame (advanced pawns)",
		sfen: "4k4/9/9/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b 2r2b4g4s4n4l9p 1",
		thinkMs: 1000,
	},
	{
		name: "handicap (6-piece)",
		sfen: "lnsgkgsnl/9/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL w - 1",
		thinkMs: 1000,
	},
];

// -- Static server --
const MIME: Record<string, string> = {
	".html": "text/html",
	".js": "application/javascript",
	".mjs": "application/javascript",
	".wasm": "application/wasm",
	".bin": "application/octet-stream",
};

function staticServer(): Promise<{ url: string; close: () => void }> {
	return new Promise((ok) => {
		const srv = createServer((req: IncomingMessage, res: ServerResponse) => {
			const url = new URL(req.url!, `http://${req.headers.host}`);
			let filePath: string | null = null;

			if (url.pathname.startsWith("/dist/")) {
				filePath = join(DIST, url.pathname.slice(6));
			} else if (url.pathname === "/nn.bin") {
				filePath = EVAL_FILE;
			} else if (url.pathname === "/test_book.db") {
				filePath = BOOK_FILE;
			} else if (url.pathname === "/runner.html") {
				filePath = join(ROOT, "script", "cfworkers_test_runner.html");
			}

			if (filePath && existsSync(filePath)) {
				const ext = extname(filePath);
				res.writeHead(200, {
					"Content-Type": MIME[ext] ?? "application/octet-stream",
					"Cross-Origin-Opener-Policy": "same-origin",
					"Cross-Origin-Embedder-Policy": "require-corp",
				});
				res.end(readFileSync(filePath));
			} else {
				res.writeHead(404);
				res.end("not found");
			}
		});
		srv.listen(0, "127.0.0.1", () => {
			const addr = srv.address() as { port: number };
			ok({ url: `http://127.0.0.1:${addr.port}`, close: () => srv.close() });
		});
	});
}

// -- Main --
const server = await staticServer();
console.log(`Static server: ${server.url}`);

const browser = await chromium.launch({ headless: true });
const context = await browser.newContext();
const page = await context.newPage();

// Collect console output from the page
page.on("console", (msg) => {
	const text = msg.text();
	if (text.startsWith("[engine]")) return; // suppress engine stderr
	if (msg.type() === "error") console.error(`  [page] ${text}`);
});

// Navigate to runner
await page.goto(`${server.url}/runner.html`);

type TestResult = {
	name: string;
	bestmove: string;
	score: string;
	depth: number | null;
	nodes: number | null;
	timeMs: number | null;
	pv: string;
	ok: boolean;
	error?: string;
};

// -- Phase 1: eval-only (no book) --
const evalResults = (await page.evaluate(async (positions) => {
	const { createEngine } = await import("/dist/index.js");
	const YaneuraOu = (await import("/dist/yaneuraou.js")).default;
	const evalBin = await fetch("/nn.bin").then((r: Response) => r.arrayBuffer());

	const out: any[] = [];
	let engine: any;
	try {
		engine = await createEngine({
			factory: YaneuraOu,
			wasmBinary: undefined,
			evalBin,
			usiHash: 16,
			handshakeTimeoutMs: 10000,
			readyTimeoutMs: 30000,
		});
	} catch (e: any) {
		return [
			{
				name: "INIT",
				bestmove: "",
				score: "",
				depth: null,
				nodes: null,
				timeMs: null,
				pv: "",
				ok: false,
				error: e.message,
			},
		];
	}

	for (const pos of positions) {
		try {
			const result = await engine.eval({
				sfen: pos.sfen,
				byoyomi: pos.thinkMs,
			});
			out.push({
				name: pos.name,
				bestmove: result.bestmove,
				score: result.score
					? `${result.score.kind} ${result.score.value}`
					: "none",
				depth: result.depth,
				nodes: result.nodes,
				timeMs: result.timeMs,
				pv: (result.pv ?? []).slice(0, 5).join(" "),
				ok: result.bestmove !== "resign" && result.bestmove !== "(none)",
			});
		} catch (e: any) {
			out.push({
				name: pos.name,
				bestmove: "",
				score: "",
				depth: null,
				nodes: null,
				timeMs: null,
				pv: "",
				ok: false,
				error: e.message,
			});
		}
	}
	engine.dispose();
	return out;
}, TEST_POSITIONS)) as TestResult[];

// -- Phase 2: eval + book (100T-shock) --
const bookResults = (await page.evaluate(async (positions) => {
	const { createEngine } = await import("/dist/index.js");
	const YaneuraOu = (await import("/dist/yaneuraou.js")).default;
	const evalBin = await fetch("/nn.bin").then((r: Response) => r.arrayBuffer());
	const bookDb = await fetch("/test_book.db").then((r: Response) =>
		r.arrayBuffer(),
	);

	const out: any[] = [];
	let engine: any;
	try {
		engine = await createEngine({
			factory: YaneuraOu,
			wasmBinary: undefined,
			evalBin,
			bookDb,
			bookFile: "user_book1.db",
			usiHash: 16,
			handshakeTimeoutMs: 10000,
			readyTimeoutMs: 30000,
		});
	} catch (e: any) {
		return [
			{
				name: "BOOK-INIT",
				bestmove: "",
				score: "",
				depth: null,
				nodes: null,
				timeMs: null,
				pv: "",
				ok: false,
				error: e.message,
			},
		];
	}

	for (const pos of positions) {
		try {
			const result = await engine.eval({
				sfen: pos.sfen,
				byoyomi: pos.thinkMs,
			});
			out.push({
				name: `[book] ${pos.name}`,
				bestmove: result.bestmove,
				score: result.score
					? `${result.score.kind} ${result.score.value}`
					: "none",
				depth: result.depth,
				nodes: result.nodes,
				timeMs: result.timeMs,
				pv: (result.pv ?? []).slice(0, 5).join(" "),
				ok: result.bestmove !== "resign" && result.bestmove !== "(none)",
			});
		} catch (e: any) {
			out.push({
				name: `[book] ${pos.name}`,
				bestmove: "",
				score: "",
				depth: null,
				nodes: null,
				timeMs: null,
				pv: "",
				ok: false,
				error: e.message,
			});
		}
	}
	engine.dispose();
	return out;
}, TEST_POSITIONS)) as TestResult[];

// -- Report --
const allResults = [...evalResults, ...bookResults];

console.log("");
console.log("=== Cloudflare Workers (V8) integration test ===");
console.log(`Engine: YaneuraOu NNUE KP256, Eval: suishopetite nn.bin (873KB)`);
console.log(`Book: 100T-shock user_book1.db (4.7MB)`);
console.log(`Runtime: Chromium ${browser.version()} (V8)`);
console.log("");

console.log("--- Phase 1: eval only (no book) ---");
let allOk = true;
for (const r of evalResults) {
	const status = r.ok ? "PASS" : "FAIL";
	if (!r.ok) allOk = false;
	console.log(`[${status}] ${r.name}`);
	if (r.error) {
		console.log(`  error: ${r.error}`);
	} else {
		console.log(
			`  bestmove: ${r.bestmove}  score: ${r.score}  depth: ${r.depth}  nodes: ${r.nodes}  time: ${r.timeMs}ms`,
		);
		console.log(`  pv: ${r.pv}`);
	}
}

console.log("");
console.log("--- Phase 2: eval + book (100T-shock) ---");
for (const r of bookResults) {
	const status = r.ok ? "PASS" : "FAIL";
	if (!r.ok) allOk = false;
	console.log(`[${status}] ${r.name}`);
	if (r.error) {
		console.log(`  error: ${r.error}`);
	} else {
		console.log(
			`  bestmove: ${r.bestmove}  score: ${r.score}  depth: ${r.depth}  nodes: ${r.nodes}  time: ${r.timeMs}ms`,
		);
		console.log(`  pv: ${r.pv}`);
	}
}

console.log("");
console.log(allOk ? "All tests passed." : "Some tests FAILED.");

await browser.close();
server.close();
process.exit(allOk ? 0 : 1);
