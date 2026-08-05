#!/usr/bin/env node
/**
 * USI エンジン同士の対局マッチ
 *
 * 開始局面集からランダムに N 局面を取り、各局面で先後を入れ替えて 2 局ずつ指す。
 * (同じ局面を両方の手番で戦うので、開始局面の有利不利が相殺される)
 *
 * Usage:
 *   node script/match.mjs \
 *     --engine1 "name=/path/to/engineA" --eval1 /path/to/evalDirA \
 *     --engine2 "name=/path/to/engineB" --eval2 /path/to/evalDirB \
 *     --sfens assets/position/start_sfens_ply32.txt \
 *     --positions 50 --movetime 500 --threads 1 --concurrency 6
 *
 * Options:
 *   --option1 / --option2  "Name=Value" を追加で送る (複数回指定可)
 *   --hash <mb>            USI_Hash (既定 64)
 *   --maxply <n>           この手数を超えたら引き分け扱い (既定 320)
 *   --seed <n>             局面抽選の乱数シード (既定 1)
 *   --json <path>          結果を JSON で書き出す
 */

import { spawn } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

// ---------------------------------------------------------------- args

const argv = process.argv.slice(2);
const opt = {
	engines: [],
	evals: [],
	options: [[], []],
	sfens: null,
	positions: 50,
	movetime: 500,
	threads: 1,
	hash: 64,
	maxply: 320,
	concurrency: 6,
	seed: 1,
	json: null,
};
for (let i = 0; i < argv.length; i++) {
	const a = argv[i];
	const next = () => argv[++i];
	const named = (v) => {
		const eq = v.indexOf("=");
		return eq === -1
			? { name: v, path: v }
			: { name: v.slice(0, eq), path: v.slice(eq + 1) };
	};
	if (a === "--engine1") opt.engines[0] = named(next());
	else if (a === "--engine2") opt.engines[1] = named(next());
	else if (a === "--eval1") opt.evals[0] = resolve(next());
	else if (a === "--eval2") opt.evals[1] = resolve(next());
	else if (a === "--option1") opt.options[0].push(next());
	else if (a === "--option2") opt.options[1].push(next());
	else if (a === "--sfens") opt.sfens = resolve(next());
	else if (a === "--positions") opt.positions = Number(next());
	else if (a === "--movetime") opt.movetime = Number(next());
	else if (a === "--threads") opt.threads = Number(next());
	else if (a === "--hash") opt.hash = Number(next());
	else if (a === "--maxply") opt.maxply = Number(next());
	else if (a === "--concurrency") opt.concurrency = Number(next());
	else if (a === "--seed") opt.seed = Number(next());
	else if (a === "--json") opt.json = next();
	else throw new Error(`unknown option: ${a}`);
}
if (!opt.engines[0] || !opt.engines[1])
	throw new Error("--engine1 and --engine2 are required");
if (!opt.sfens) throw new Error("--sfens is required");

// 再現性のある擬似乱数 (xorshift32)
function makeRng(seed) {
	let x = seed >>> 0 || 1;
	return () => {
		x ^= x << 13;
		x >>>= 0;
		x ^= x >> 17;
		x ^= x << 5;
		x >>>= 0;
		return x / 4294967296;
	};
}

// ---------------------------------------------------------------- engine

// USI エンジンを1つ起動して、行単位で読み書きする薄いラッパー。
class Engine {
	constructor(spec, evalDir, extraOptions, o) {
		this.spec = spec;
		this.proc = spawn(spec.path, { stdio: ["pipe", "pipe", "ignore"] });
		this.buf = "";
		this.lines = [];
		this.waiters = [];
		this.proc.stdout.on("data", (b) => {
			this.buf += b.toString();
			const parts = this.buf.split("\n");
			this.buf = parts.pop();
			for (const raw of parts) {
				const line = raw.trim();
				if (!line) continue;
				this.lines.push(line);
				for (const w of [...this.waiters])
					if (w.re.test(line)) {
						this.waiters.splice(this.waiters.indexOf(w), 1);
						w.resolve(line);
					}
			}
		});
		this.evalDir = evalDir;
		this.extraOptions = extraOptions;
		this.o = o;
	}

	send(cmd) {
		this.proc.stdin.write(`${cmd}\n`);
	}

	// 行が来るのを待つ。既に来ている場合も拾う。
	wait(re, ms) {
		const hit = this.lines.find((l) => re.test(l));
		if (hit) return Promise.resolve(hit);
		return new Promise((resolve, reject) => {
			const w = { re, resolve };
			this.waiters.push(w);
			setTimeout(() => {
				const i = this.waiters.indexOf(w);
				if (i >= 0) {
					this.waiters.splice(i, 1);
					reject(new Error(`timeout ${re} (${this.spec.name})`));
				}
			}, ms).unref();
		});
	}

	async init() {
		this.send("usi");
		await this.wait(/^usiok/, 30_000);
		if (this.evalDir) this.send(`setoption name EvalDir value ${this.evalDir}`);
		this.send(`setoption name Threads value ${this.o.threads}`);
		this.send(`setoption name USI_Hash value ${this.o.hash}`);
		this.send("setoption name USI_OwnBook value false");
		this.send("setoption name NetworkDelay value 0");
		this.send("setoption name NetworkDelay2 value 0");
		this.send("setoption name MinimumThinkingTime value 0");
		this.send("setoption name PvInterval value 0");
		for (const kv of this.extraOptions) {
			const eq = kv.indexOf("=");
			this.send(`setoption name ${kv.slice(0, eq)} value ${kv.slice(eq + 1)}`);
		}
		this.send("isready");
		await this.wait(/^readyok/, 300_000);
	}

	async go(sfen, moves, movetime) {
		this.lines.length = 0;
		this.send(
			`position sfen ${sfen}${moves.length ? ` moves ${moves.join(" ")}` : ""}`,
		);
		this.send(`go movetime ${movetime}`);
		const line = await this.wait(/^bestmove/, movetime * 20 + 60_000);
		return line.split(/\s+/)[1];
	}

	quit() {
		try {
			this.send("quit");
			setTimeout(() => this.proc.kill("SIGKILL"), 1000).unref();
		} catch {}
	}
}

// ---------------------------------------------------------------- game

/**
 * 1局指す。engines[0] が先手。
 * 戻り値: 0 = engines[0] の勝ち / 1 = engines[1] の勝ち / null = 引き分け
 */
async function playGame(engines, sfen, o) {
	const moves = [];
	for (let ply = 0; ply < o.maxply; ply++) {
		const side = ply % 2; // 0 = 先手側エンジン
		const bestmove = await engines[side].go(sfen, moves, o.movetime);

		// 投了・入玉宣言。宣言勝ちは勝ち、投了は負け。
		if (bestmove === "resign") return 1 - side;
		if (bestmove === "win") return side;
		if (!bestmove || bestmove === "(none)") return 1 - side;

		moves.push(bestmove);
	}
	return null; // 手数上限 = 引き分け
}

// ---------------------------------------------------------------- main

const allSfens = readFileSync(opt.sfens, "utf8")
	.split("\n")
	.map((l) => l.trim())
	.filter((l) => l.startsWith("sfen "))
	.map((l) => l.slice("sfen ".length));

const rng = makeRng(opt.seed);
const picked = [];
const used = new Set();
while (picked.length < opt.positions && used.size < allSfens.length) {
	const i = Math.floor(rng() * allSfens.length);
	if (used.has(i)) continue;
	used.add(i);
	picked.push(allSfens[i]);
}

// 1局面につき先後入れ替えで2局。
const jobs = [];
for (let i = 0; i < picked.length; i++) {
	jobs.push({ id: jobs.length, sfen: picked[i], swap: false });
	jobs.push({ id: jobs.length, sfen: picked[i], swap: true });
}

const results = [];
let done = 0;
const t0 = Date.now();

async function worker() {
	// ワーカーごとにエンジンを1組起動して使い回す。
	const es = [
		new Engine(opt.engines[0], opt.evals[0], opt.options[0], opt),
		new Engine(opt.engines[1], opt.evals[1], opt.options[1], opt),
	];
	await Promise.all(es.map((e) => e.init()));

	try {
		for (;;) {
			const job = jobs.shift();
			if (!job) break;
			// swap のときは engine2 が先手。
			const order = job.swap ? [es[1], es[0]] : [es[0], es[1]];
			let winnerSeat;
			try {
				winnerSeat = await playGame(order, job.sfen, opt);
			} catch (e) {
				console.error(`game ${job.id} failed: ${e.message}`);
				winnerSeat = undefined;
			}
			// 席順を「エンジン番号」に戻す。
			const winner =
				winnerSeat === null || winnerSeat === undefined
					? winnerSeat
					: job.swap
						? 1 - winnerSeat
						: winnerSeat;
			results.push({ id: job.id, swap: job.swap, winner });
			done++;
			const w = results.filter((r) => r.winner === 0).length;
			const l = results.filter((r) => r.winner === 1).length;
			const d = results.filter((r) => r.winner === null).length;
			const el = ((Date.now() - t0) / 1000).toFixed(0);
			process.stdout.write(
				`\r${done}/${jobs.length + done} 局  ${opt.engines[0].name} ${w}勝 - ${l}敗 - ${d}分  (${el}s)   `,
			);
		}
	} finally {
		for (const e of es) e.quit();
	}
}

const total = jobs.length;
console.log(
	`${opt.engines[0].name} vs ${opt.engines[1].name} : ${total} 局 ` +
		`(${picked.length} 局面 × 先後) / ${opt.movetime}ms / Threads=${opt.threads} / 並列 ${opt.concurrency}`,
);

await Promise.all(Array.from({ length: opt.concurrency }, () => worker()));

const win = results.filter((r) => r.winner === 0).length;
const loss = results.filter((r) => r.winner === 1).length;
const draw = results.filter((r) => r.winner === null).length;
const err = results.filter((r) => r.winner === undefined).length;
const decided = win + loss;
const rate = decided ? win / decided : 0;
// 勝率から Elo 差。引き分けは除外して計算する。
const elo =
	decided === 0 || rate === 0 || rate === 1
		? null
		: -400 * Math.log10(1 / rate - 1);
// 二項分布の標準誤差から 95% 信頼区間。
const se = decided ? Math.sqrt((rate * (1 - rate)) / decided) : 0;
const loRate = Math.max(0.0001, rate - 1.96 * se);
const hiRate = Math.min(0.9999, rate + 1.96 * se);
const eloLo = decided ? -400 * Math.log10(1 / loRate - 1) : null;
const eloHi = decided ? -400 * Math.log10(1 / hiRate - 1) : null;

console.log(`\n\n=== 結果 ===`);
console.log(
	`${opt.engines[0].name} : ${win}勝 ${loss}敗 ${draw}分${err ? ` (${err}局エラー)` : ""}`,
);
console.log(`勝率 (引き分け除く) : ${(rate * 100).toFixed(1)}%`);
if (elo !== null)
	console.log(
		`Elo 差 : ${elo >= 0 ? "+" : ""}${elo.toFixed(0)} [${eloLo.toFixed(0)}, ${eloHi.toFixed(0)}] (95%)`,
	);
console.log(`所要 : ${((Date.now() - t0) / 60000).toFixed(1)} 分`);

if (opt.json) {
	writeFileSync(
		opt.json,
		`${JSON.stringify({ opt: { ...opt, engines: opt.engines }, win, loss, draw, err, rate, elo, results }, null, 2)}\n`,
	);
	console.log(`wrote ${opt.json}`);
}
process.exit(0);
