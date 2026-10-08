import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { stdout } from 'node:process';
import test from 'node:test';
import { runCli } from '../src/agent-sdk/cli.js';
import { createFilePairingStore, PAIRING_TTL_MS } from '../src/agent-sdk/pairing-store.js';

const entry = (id) => `${id}:0123456789abcdef0123456789abcdef`;

function directory(t) {
  const path = mkdtempSync(join(tmpdir(), 'kubik-pairing-cli-'));
  t.after(() => rmSync(path, { recursive: true, force: true }));
  return path;
}

async function pairList(t, stateDir, onWrite = () => {}) {
  let output = '';
  const write = t.mock.method(stdout, 'write', (chunk) => {
    output += chunk;
    onWrite();
    return true;
  });
  try {
    await runCli(['--state-dir', stateDir, 'pair-list'], {});
    return output;
  } finally { write.mock.restore(); }
}

test('pair-list shows remaining validity without renewing the code or saved request', async (t) => {
  const stateDir = directory(t);
  const createdAt = 1_000_000_000;
  let now = createdAt;
  t.mock.method(Date, 'now', () => now);
  const pairing = await createFilePairingStore(stateDir, { randomCode: () => 'ABCDEFG2' }).ready();
  await pairing.upsert(entry('desk'), { name: 'Desk' });
  const saved = readFileSync(pairing.path, 'utf8');
  const cases = [
    [0, '24h 0m 0s'],
    [1001, '23h 59m 59s'],
    [3_661_000, '22h 58m 59s'],
    [PAIRING_TTL_MS - 60_001, '0h 1m 1s'],
    [PAIRING_TTL_MS - 59_001, '0h 1m 0s'],
    [PAIRING_TTL_MS - 1, '0h 0m 1s'],
    [PAIRING_TTL_MS, '0h 0m 0s'],
  ];
  for (const [age, remaining] of cases) {
    now = createdAt + age;
    assert.equal(await pairList(t, stateDir),
      `Pending: ${entry('desk')} (Desk), code ABCDEFG2, expires in ${remaining}\n`);
    assert.equal(readFileSync(pairing.path, 'utf8'), saved);
  }
  now++;
  assert.equal(await pairList(t, stateDir), 'No pending or approved devices.\n');
  assert.equal(readFileSync(pairing.path, 'utf8'), saved, 'listing expiry must not rewrite the store');
});

test('pair-list hides expired requests and keeps approved devices without a countdown', async (t) => {
  const stateDir = directory(t);
  let now = 1_000_000_000;
  t.mock.method(Date, 'now', () => now);
  const codes = ['ABCDEFG2', 'ABCDEFG3', 'ABCDEFG4'];
  const pairing = await createFilePairingStore(stateDir, { randomCode: () => codes.shift() }).ready();
  await pairing.upsert(entry('expired'));
  await pairing.upsert(entry('approved'));
  await pairing.approve('ABCDEFG3');
  now++;
  await pairing.upsert(entry('boundary'), { name: '' });
  const saved = readFileSync(pairing.path, 'utf8');
  now += PAIRING_TTL_MS;
  assert.equal(await pairList(t, stateDir),
    `Pending: ${entry('boundary')} (unnamed), code ABCDEFG4, expires in 0h 0m 0s\nApproved: ${entry('approved')}\n`);
  assert.equal(readFileSync(pairing.path, 'utf8'), saved);
  await pairing.approve('ABCDEFG4');
  assert.equal(await pairList(t, stateDir), `Approved: ${entry('approved')}\nApproved: ${entry('boundary')}\n`);
});

test('pair-list uses one time snapshot for all pending rows', async (t) => {
  const stateDir = directory(t);
  let now = 1_000_000_000;
  t.mock.method(Date, 'now', () => now);
  const codes = ['ABCDEFG2', 'ABCDEFG3'];
  const pairing = await createFilePairingStore(stateDir, { randomCode: () => codes.shift() }).ready();
  await pairing.upsert(entry('first'));
  await pairing.upsert(entry('second'));
  const output = await pairList(t, stateDir, () => { now += 3_600_000; });
  assert.equal(output,
    `Pending: ${entry('first')} (first), code ABCDEFG2, expires in 24h 0m 0s\n` +
    `Pending: ${entry('second')} (second), code ABCDEFG3, expires in 24h 0m 0s\n`);
});

test('pair-list caps a request newer than the view clock at its 24-hour lifetime', async (t) => {
  const stateDir = directory(t);
  let now = 1_000_001_001;
  t.mock.method(Date, 'now', () => now);
  const pairing = await createFilePairingStore(stateDir, { randomCode: () => 'ABCDEFG2' }).ready();
  await pairing.upsert(entry('new'));
  const saved = readFileSync(pairing.path, 'utf8');
  // A concurrent insertion or clock adjustment can make createdAt later than listedAt.
  now -= 1001;
  assert.equal(await pairList(t, stateDir),
    `Pending: ${entry('new')} (new), code ABCDEFG2, expires in 24h 0m 0s\n`);
  assert.equal(readFileSync(pairing.path, 'utf8'), saved);
});
