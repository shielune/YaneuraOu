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
    assert.deepEqual(Object.keys(row.parts).sort(), ['activity', 'king', 'material', 'speed', 'tempo']);
    for (const value of Object.values(row.parts)) assert(typeof value === 'number' && Number.isFinite(value));
  }
}
console.log('native explain: root, quiet moves, capture/promotion and five parts passed');
