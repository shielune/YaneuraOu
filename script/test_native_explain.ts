// No isready/go: explain must work without an NNUE evaluation file.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { resolve } from 'node:path';

const [binary, models] = process.argv.slice(2);
assert(binary && models, 'usage: node script/test_native_explain.ts <binary> <model-directory>');
for (const moves of [['7g7f', '3c3d'], ['7g7f', '3c3d', '8h2b+', '3a2b']]) {
  const commands = [
    'usi',
    `setoption name HceProgressFile value ${resolve(models, 'hce-progress.bin')}`,
    `setoption name HceExplainFile value ${resolve(models, 'hce-explain.bin')}`,
    'position startpos',
    `explain moves ${moves.join(' ')}`,
    'quit',
    '',
  ];
  const result = spawnSync(resolve(binary), [], {
    input: commands.join('\n'), encoding: 'utf8', timeout: 120_000,
    maxBuffer: 4 * 1024 * 1024,
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /usiok/);
  assert.match(result.stdout, /HceProgressFile: loaded /);
  assert.match(result.stdout, /HceExplainFile: loaded /);
  const rows = result.stdout.split(/\r?\n/).filter(line => line.startsWith('{"ply":')).map(line => JSON.parse(line));
  assert.equal(rows.length, moves.length + 1, result.stdout);
  for (const [ply, row] of rows.entries()) {
    assert.equal(row.ply, ply);
    if (ply) assert.equal(row.move, moves[ply - 1]);
    assert.deepEqual(Object.keys(row.parts).sort(), ['king_safety', 'material', 'mobility', 'tempo', 'threat']);
    for (const value of Object.values(row.parts)) assert(typeof value === 'number' && Number.isFinite(value));
  }
}

{
  const run = (explain: string) => {
    const result = spawnSync(resolve(binary), [], {
      input: ['usi', `setoption name HceProgressFile value ${resolve(models, 'hce-progress.bin')}`,
        `setoption name HceExplainFile value ${resolve(models, 'hce-explain.bin')}`,
        'position startpos', explain, 'quit', ''].join('\n'),
      encoding: 'utf8', timeout: 120_000, maxBuffer: 4 * 1024 * 1024,
    });
    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert(!/[^\x00-\x7F]/.test(result.stdout), 'explain output must be ASCII');
    const lines = result.stdout.split(/\r?\n/);
    assert(!lines.some(l => l.includes('Error!')), result.stdout);
    return lines.filter(l => l.startsWith('{')).map(l => JSON.parse(l));
  };
  const plain = run('explain moves 7g7f 3c3d');
  assert.deepEqual(plain.at(-1), { done: true, positions: 3 });
  assert.deepEqual(plain[0].parts, ['material', 'tempo', 'threat', 'king_safety', 'mobility']);
  for (const row of plain.filter(r => 'ply' in r))
    for (const key of ['black_pov', 'linear_material', 'linear_mobility', 'top', 'd_linear_material']) assert(!(key in row), key);
  const legacy = run('explain legacy top 3 moves 7g7f 3c3d');
  assert.equal(legacy[0].route, 'linear');
  assert.equal(legacy.at(-1).positions, 3);
  assert('top' in legacy[2] && legacy[2].top.length <= 3);
  assert.deepEqual(run('explain legacy top 0 moves 7g7f')[2].top, []);
  const bad = run('explain moves 7g7f 7g7f');
  assert.deepEqual(bad.at(-2), { ply: 2, move: '7g7f', error: 'illegal move' });
  assert.deepEqual(bad.at(-1), { done: true, positions: 2, error: true });
}

// go explain needs an NNUE evaluation file: only run when EVAL_DIR is set.
if (process.env.EVAL_DIR) {
  const go = (cmds: string[]) => {
    const result = spawnSync(resolve(binary), [], {
      input: ['usi', `setoption name EvalDir value ${resolve(process.env.EVAL_DIR!)}`,
        'setoption name USI_OwnBook value false', 'setoption name MultiPV value 3',
        `setoption name HceProgressFile value ${resolve(models, 'hce-progress.bin')}`,
        `setoption name HceExplainFile value ${resolve(models, 'hce-explain.bin')}`,
        'isready', ...cmds, 'quit', ''].join('\n'),
      encoding: 'utf8', timeout: 120_000, maxBuffer: 16 * 1024 * 1024,
    });
    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    return result.stdout.split(/\r?\n/);
  };
  // quit right after go is racy for a short search: wait for bestmove via a long enough movetime.
  const out = go(['position startpos moves 7g7f 3c3d', 'go explain movetime 300', 'go movetime 100']);
  const done = out.findIndex(l => l.startsWith('{"done"'));
  const best = out.findIndex(l => l.startsWith('bestmove'));
  assert(done > 0 && best > done, 'bestmove must come after the done line');
  assert.equal(out.slice(best + 1).filter(l => l.startsWith('{')).length, 0, 'no JSON after the first bestmove');
  const rows = out.slice(0, done + 1).filter(l => l.startsWith('{')).map(l => JSON.parse(l));
  assert.deepEqual(rows[0].parts, ['material', 'tempo', 'threat', 'king_safety', 'mobility']);
  assert.equal(rows[0].multipv, 3);
  const body = rows.slice(1, -1);
  assert.equal(rows.at(-1).positions, body.length);
  const keys = ['king_safety', 'material', 'mobility', 'tempo', 'threat'];
  assert.equal(body[0].kind, 'root');
  assert(!('multipv' in body[0]) && !('ply' in body[0]) && Array.isArray(body[0].qsearch_moves));
  assert.deepEqual(Object.keys(body[0].parts).sort(), keys);
  assert.deepEqual(Object.keys(body[0].qsearch_parts).sort(), keys);
  if (body[0].qsearch_moves.length === 0) {
    assert.equal(body[0].qsearch_sfen, body[0].sfen);
    assert.deepEqual(body[0].qsearch_parts, body[0].parts);
  }
  let total = 1;
  for (let id = 1; id <= rows[0].multipv; id++) {
    const mine = body.filter(r => r.multipv === id);
    const pv = mine.filter(r => r.kind === 'pv');
    const qs = mine.filter(r => r.kind === 'leaf');
    assert(pv.length >= 1 && qs.length === 1, `multipv ${id}`);
    assert.deepEqual(pv.map(r => r.ply), pv.map((_, i) => i + 1));
    assert.equal(mine.at(-1), qs[0], 'leaf row comes last');
    for (const r of pv) {
      assert('see' in r && 'gives_check' in r && 'move' in r && !('qsearch_sfen' in r));
      assert.deepEqual(Object.keys(r.parts).sort(), keys);
    }
    assert.equal(qs[0].ply, pv.length);
    assert.equal(qs[0].sfen, pv.at(-1).sfen);
    assert(Array.isArray(qs[0].qsearch_moves) && typeof qs[0].qsearch_sfen === 'string');
    assert.deepEqual(Object.keys(qs[0].parts).sort(), keys);
    if (qs[0].qsearch_moves.length === 0) assert.deepEqual(qs[0].parts, pv.at(-1).parts);
    total += mine.length;
  }
  assert.equal(body.length, total);
  assert.equal(out.filter(l => l.startsWith('{"done"')).length, 1, 'plain go must not explain');
}
console.log('native explain: root, quiet moves, capture/promotion, five parts, legacy, top 0 and illegal move passed');
