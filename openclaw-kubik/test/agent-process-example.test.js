import assert from 'node:assert/strict';
import test from 'node:test';
import { chmodSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import adapter from '../src/agent-sdk/examples/local-process.js';

async function fixture(t) {
  const directory = mkdtempSync(join(tmpdir(), "kubik process's "));
  const command = join(directory, 'agent runner.js');
  writeFileSync(command, `#!${process.execPath}
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
const directory = ${JSON.stringify(directory)};
const id = process.argv[3];
writeFileSync(join(directory, id), String(process.pid));
if (id === 'broken-input') {
  process.stdin.destroy(); process.stdout.end('Must not succeed');
} else {
  let text = '';
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', chunk => text += chunk);
  process.stdin.on('end', () => {
    if (text === 'hold') { process.on('SIGTERM', () => {}); setInterval(() => {}, 1000); }
    else if (text === 'bytes') process.stdout.end('x'.repeat(65_537));
    else if (text === 'chars') process.stdout.end('x'.repeat(8193));
    else if (text === 'empty') process.stdout.end();
    else if (text === 'error') { process.stderr.write('private provider data'); process.exit(2); }
    else process.stdout.end(JSON.stringify({ id, text }));
  });
}
`);
  chmodSync(command, 0o700);
  t.after(async () => { await adapter.close(); rmSync(directory, { recursive: true, force: true }); });
  await adapter.connect({ config: { command } });
  const spoken = [], errors = [];
  const turn = (id, transcript) => adapter.dispatch({ device: { id }, transcript,
    isCurrent: () => true, speak: async text => spoken.push(text), onAgentError: error => errors.push(error.message) });
  return { directory, command, spoken, errors, turn };
}

async function waitFor(predicate) {
  for (let i = 0; i < 500; i++) {
    if (predicate()) return;
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  throw new Error('agent fixture did not start');
}

function processGone(directory, id) {
  const pid = Number(readFileSync(join(directory, id), 'utf8'));
  assert.throws(() => process.kill(pid, 0), { code: 'ESRCH' });
}

const posix = { skip: process.platform === 'win32' };

test('process example passes literal stdin and isolated sessions without a shell', posix, async t => {
  const { turn, spoken, errors } = await fixture(t);
  await turn('one', 'Hello; $(touch unwanted)');
  await turn('two', 'World');
  assert.deepEqual(spoken.map(JSON.parse), [
    { id: 'one', text: 'Hello; $(touch unwanted)' }, { id: 'two', text: 'World' },
  ]);
  assert.deepEqual(errors, []);
});

test('process example rejects failed starts, stdin, exits and oversized or empty replies', posix, async t => {
  const { turn, command, spoken, errors } = await fixture(t);
  for (const transcript of ['error', 'bytes', 'chars', 'empty']) await turn('bounds', transcript);
  await turn('broken-input', 'x'.repeat(1024 * 1024));
  rmSync(command);
  await turn('missing', 'hello');
  assert.deepEqual(spoken, []);
  assert.deepEqual(errors, [
    'Agent process failed', 'Agent process failed', 'Invalid reply', 'Invalid reply',
    'Agent process failed', 'Agent process failed',
  ]);
});

test('process example bounds concurrency, kills on close, and reconnects cleanly', posix, async t => {
  const { turn, command, directory, spoken, errors } = await fixture(t);
  const ids = Array.from({ length: 8 }, (_, i) => `held-${i}`);
  const jobs = ids.map(id => turn(id, 'hold'));
  await waitFor(() => ids.every(id => existsSync(join(directory, id))));
  await turn('ninth', 'hold');
  assert.deepEqual(errors, ['Agent is busy']);
  assert.equal(existsSync(join(directory, 'ninth')), false);
  await adapter.close();
  await Promise.all(jobs);
  for (const id of ids) processGone(directory, id);
  await turn('already-closed', 'hello');
  assert.equal(existsSync(join(directory, 'already-closed')), false);
  await adapter.connect({ config: { command } });
  await turn('reconnected', 'hello');
  assert.deepEqual(spoken.map(JSON.parse), [{ id: 'reconnected', text: 'hello' }]);
});

test('process example enforces its deadline even when SIGTERM is ignored',
  { ...posix, timeout: 35_000 }, async t => {
    const { turn, directory, spoken, errors } = await fixture(t);
    const started = Date.now();
    await turn('timeout', 'hold');
    assert.ok(Date.now() - started >= 29_000);
    assert.deepEqual(spoken, []);
    assert.deepEqual(errors, ['Agent process failed']);
    processGone(directory, 'timeout');
  });

test('process example probes an absolute executable before serving', async () => {
  await assert.rejects(adapter.connect({ config: { command: 'relative-agent' } }), /absolute executable/);
  await assert.rejects(adapter.connect({ config: { command: join(tmpdir(), 'missing-kubik-agent') } }), /unavailable/);
  await adapter.close();
});
