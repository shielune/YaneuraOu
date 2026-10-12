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
console.log('native explain: root, quiet moves, capture/promotion, five parts, legacy, top 0 and illegal move passed');
