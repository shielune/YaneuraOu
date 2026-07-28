#!/usr/bin/env node
/**
 * 変種横断のノード固定ベンチマーク
 *
 * 同じ局面を同じノード数だけ読ませて、かかった時間を比べる。
 * 絶対値はマシン依存だが、同一マシンで走らせる限り変種間の比較になる。
 *
 * 探索を決定的にするため、全変種で以下を固定する:
 *   Threads=1 / USI_Hash 固定 / usinewgame で置換表クリア / 定跡オフ
 * これらが揃っていれば、評価関数の実装が同じ変種同士は
 * bestmove どころか探索ノード数まで一致する。逆に一致しなければ
 * どこかの評価経路が壊れている。
 *
 * Usage:
 *   node script/bench_nodes.mjs --config script/bench_nodes.config.json
 *   node script/bench_nodes.mjs --native <path> --wasm <dir> ...
 *
 * Options:
 *   --sfen <sfen>     "position" に渡す文字列 (既定: 下の DEFAULT_SFEN)
 *   --nodes <n>       読ませるノード数 (既定: 1000000)
 *   --hash <mb>       USI_Hash (既定: 16 — edge 変種の 128MB ヒープに収まる値)
 *   --eval <path>     nn.bin の場所。NNUE 系エディションでは必須
 *   --repeat <n>      各変種を n 回走らせて中央値を採る (既定: 1)
 *   --native <path>   ネイティブ実行ファイル。name=path 形式でラベルを付けられる
 *   --wasm <dir>      yaneuraou.js のあるディレクトリ。name=dir 形式可
 *   --json <path>     結果を JSON で書き出す
 */

import { spawn } from "node:child_process";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const DEFAULT_SFEN =
	"sfen l1g1k2nl/1r4g2/2nsppsp1/p1pp2p1p/1p4PP1/P1P2P2P/1PSPPS3/2GK1G1R1/LN5NL w Bb 32";

// ---------------------------------------------------------------- args

function parseArgs(argv) {
	const o = {
		sfen: DEFAULT_SFEN,
		nodes: 1_000_000,
		hash: 16,
		repeat: 1,
		eval: null,
		json: null,
		targets: [],
	};
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		const next = () => argv[++i];
		if (a === "--sfen") o.sfen = next();
		else if (a === "--nodes") o.nodes = Number(next());
		else if (a === "--go") o.go = next();
		else if (a === "--hash") o.hash = Number(next());
		else if (a === "--repeat") o.repeat = Number(next());
		else if (a === "--eval") o.eval = resolve(next());
		else if (a === "--json") o.json = next();
		else if (a === "--native" || a === "--wasm") {
			const kind = a === "--native" ? "native" : "wasm";
			const v = next();
			const eq = v.indexOf("=");
			const [label, path] =
				eq === -1 ? [null, v] : [v.slice(0, eq), v.slice(eq + 1)];
			o.targets.push({ kind, path: resolve(path), label: label ?? path });
		} else if (a === "--config") {
			const cfg = JSON.parse(readFileSync(next(), "utf8"));
			Object.assign(o, cfg, {
				targets: [...o.targets, ...(cfg.targets ?? [])],
			});
		} else throw new Error(`unknown option: ${a}`);
	}
	if (o.targets.length === 0)
		throw new Error("no --native / --wasm target given");
	return o;
}

// USI コマンド列。全変種で共通。
function commands(opt) {
	return [
		"usi",
		`setoption name USI_Hash value ${opt.hash}`,
		"setoption name Threads value 1",
		"setoption name USI_OwnBook value false",
		// ⚠ PvInterval を 0 にしないと、既定の 300ms 間引きのせいで
		//   「最後に出力された info 行」が速い変種と遅い変種で別の反復のものになり、
		//   探索は同一なのに depth / score が食い違って見える。
		"setoption name PvInterval value 0",
		...(opt.eval ? [`setoption name EvalDir value ${opt.evalDir}`] : []),
		"isready",
		"usinewgame",
		`position ${opt.sfen}`,
		// 既定は "nodes N"。--go で "depth 10" などに差し替えられる。
		`go ${opt.go ?? `nodes ${opt.nodes}`}`,
	];
}

// 最後の "info depth" 行から数値を拾う。
function parseInfo(lines) {
	const info = lines.filter((l) => l.startsWith("info depth")).pop() ?? "";
	const num = (key) => {
		const m = info.match(new RegExp(`\\b${key} (-?\\d+)`));
		return m ? Number(m[1]) : null;
	};
	const score = info.match(/score (cp|mate) (-?\d+)/);
	return {
		depth: num("depth"),
		seldepth: num("seldepth"),
		nodes: num("nodes"),
		nps: num("nps"),
		timeMs: num("time"),
		score: score ? `${score[1]} ${score[2]}` : null,
		bestmove: (lines.find((l) => l.startsWith("bestmove")) ?? "").trim(),
	};
}

// ---------------------------------------------------------------- native

function runNative(target, opt) {
	return new Promise((res, rej) => {
		// 評価関数はバイナリと同じ階層の EvalDir から読まれるので、
		// バイナリを置いたディレクトリを基準に絶対パスを渡す。
		const cmds = commands({ ...opt, evalDir: opt.evalDirNative });
		const child = spawn(target.path, { stdio: ["pipe", "pipe", "pipe"] });
		const lines = [];
		let done = false;

		const finish = (err) => {
			if (done) return;
			done = true;
			try {
				child.kill("SIGKILL");
			} catch {}
			err ? rej(err) : res(parseInfo(lines));
		};

		child.stdout.on("data", (b) => {
			for (const l of b.toString().split("\n")) {
				const t = l.trim();
				if (!t) continue;
				lines.push(t);
				// bestmove が出たら終了。stdin を閉じると探索が中断されるので、
				// それまでは開けたままにしておく。
				if (t.startsWith("bestmove")) finish();
			}
		});
		child.on("error", finish);
		child.stdin.write(`${cmds.join("\n")}\n`);
		setTimeout(() => finish(new Error("native timeout")), 600_000).unref();
	});
}

// ---------------------------------------------------------------- wasm

async function runWasm(target, opt) {
	const factory = (await import(join(target.path, "yaneuraou.js"))).default;
	const lines = [];
	const engine = await factory({});
	engine.addMessageListener((l) => lines.push(l));

	const waitFor = (re, ms) =>
		new Promise((res, rej) => {
			const t0 = Date.now();
			const iv = setInterval(() => {
				const hit = lines.find((l) => re.test(l));
				if (hit) {
					clearInterval(iv);
					res(hit);
				} else if (Date.now() - t0 > ms) {
					clearInterval(iv);
					rej(new Error(`wasm timeout ${re} | ${lines.slice(-3).join(" / ")}`));
				}
			}, 50);
		});
	const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

	engine.postMessage("usi");
	await waitFor(/usiok/, 30_000);

	// 評価関数は MEMFS に流し込む。公開パッケージと同じ手順。
	if (opt.eval) {
		engine.FS.mkdir("/eval");
		engine.FS.writeFile("/eval/nn.bin", new Uint8Array(readFileSync(opt.eval)));
	}
	for (const c of commands({ ...opt, evalDir: "/eval" }).slice(1)) {
		engine.postMessage(c);
		// setoption はハンドラで置換表確保などを伴うので、詰め込まず間を置く。
		if (c.startsWith("setoption")) await sleep(300);
		if (c === "isready") await waitFor(/readyok/, 300_000);
	}
	await waitFor(/^bestmove/, 900_000);
	return parseInfo(lines);
}

// ---------------------------------------------------------------- main

const opt = parseArgs(process.argv.slice(2));
if (opt.eval && !existsSync(opt.eval))
	throw new Error(`eval not found: ${opt.eval}`);
// ネイティブ側は EvalDir を絶対パスで受け取れるので、nn.bin の親を渡す。
opt.evalDirNative = opt.eval ? resolve(opt.eval, "..") : null;

const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];

const results = [];
for (const t of opt.targets) {
	const runs = [];
	for (let i = 0; i < opt.repeat; i++) {
		runs.push(
			t.kind === "native" ? await runNative(t, opt) : await runWasm(t, opt),
		);
	}
	const r = runs[0];
	results.push({
		label: t.label,
		kind: t.kind,
		...r,
		timeMs: median(runs.map((x) => x.timeMs)),
		nps: median(runs.map((x) => x.nps)),
		runs: runs.length,
	});
	console.log(
		`${t.label.padEnd(18)} ${String(r.nodes).padStart(9)} nodes  ` +
			`${String(median(runs.map((x) => x.timeMs))).padStart(7)} ms  ` +
			`${String(median(runs.map((x) => x.nps))).padStart(9)} nps  ` +
			`depth ${String(r.depth).padStart(2)}  ${String(r.score).padStart(9)}  ${r.bestmove}`,
	);
}

// 探索が同一かどうかの判定。ノード数と bestmove が揃っていれば同じ木を読んでいる。
const key = (r) => `${r.nodes}|${r.bestmove}|${r.score}|${r.depth}`;
const groups = new Map();
for (const r of results) {
	const k = key(r);
	groups.set(k, [...(groups.get(k) ?? []), r.label]);
}
console.log(
	`\n探索の同一性: ${groups.size === 1 ? "全変種一致" : `${groups.size} グループに分かれた`}`,
);
for (const [k, labels] of groups) console.log(`  [${labels.join(", ")}] ${k}`);

if (opt.json) {
	writeFileSync(opt.json, `${JSON.stringify({ opt, results }, null, 2)}\n`);
	console.log(`\nwrote ${opt.json}`);
}
