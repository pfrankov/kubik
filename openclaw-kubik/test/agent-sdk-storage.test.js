import assert from 'node:assert/strict';
import fs from 'node:fs';
import { chmodSync, mkdtempSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync } from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { createFilePairingStore, MAX_PENDING_PAIRINGS, PAIRING_FILE, PAIRING_TTL_MS } from '../src/agent-sdk/pairing-store.js';
import { HOST_CONFIG_FILE, readHostConfig, writeHostConfig } from '../src/agent-sdk/config.js';

const config = (port) => ({
  version: 2,
  listener: { host: '127.0.0.1', port },
  voice: { baseUrl: 'https://speech.example/v1', transcribeModel: 'stt', ttsModel: 'tts', voice: 'alloy', language: 'en' },
  adapter: { id: 'storage-test', setup: {} },
});

function stateDirectory(t) {
  const path = mkdtempSync(join(tmpdir(), 'kubik-agent-storage-'));
  t.after(() => rmSync(path, { recursive: true, force: true }));
  return path;
}

function countPairingWrites(t, path) {
  const rename = fs.renameSync;
  let writes = 0;
  const tracked = t.mock.method(fs, 'renameSync', (from, to) => {
    const result = rename(from, to);
    if (to === path) writes++;
    return result;
  });
  syncBuiltinESMExports();
  t.after(() => { tracked.mock.restore(); syncBuiltinESMExports(); });
  return () => writes;
}

test('pairing retries reuse saved state while a separate approval is immediately visible', async (t) => {
  const stateDir = stateDirectory(t);
  const pairing = await createFilePairingStore(stateDir, { randomCode: () => 'ABCDEFG2' }).ready();
  const entry = 'kubik-retry:0123456789abcdef0123456789abcdef';
  const writes = countPairingWrites(t, pairing.path);
  const request = await pairing.upsert(entry, { name: 'Desk', fw: '0.6.2' });
  assert.deepEqual(request, { code: 'ABCDEFG2', created: true });
  assert.equal(writes(), 1, 'a new request must be persisted');
  const saved = readFileSync(pairing.path, 'utf8');

  assert.deepEqual(await pairing.upsert(entry), { code: request.code, created: false });
  assert.equal(writes(), 1, 'an unchanged retry must not replace the pairing file');
  assert.equal(readFileSync(pairing.path, 'utf8'), saved);

  const approver = await createFilePairingStore(stateDir).ready();
  await approver.approve(request.code);
  assert.equal(writes(), 2, 'approval must be persisted');
  assert.deepEqual(await pairing.allowed(), [entry]);
  assert.deepEqual(await pairing.upsert(entry), { code: '', created: false });
  assert.equal(writes(), 2, 'an approved request must not rewrite the file');
});

test('a full unchanged pairing queue rejects a request without rewriting saved state', async (t) => {
  const stateDir = stateDirectory(t);
  const now = 100_000;
  const pairing = await createFilePairingStore(stateDir, { now: () => now }).ready();
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  const pending = Array.from({ length: MAX_PENDING_PAIRINGS }, (_, index) => ({
    entry: `device-${index}:0123456789abcdef0123456789abcdef`,
    code: `AAAAAA${alphabet[Math.floor(index / 32)]}${alphabet[index % 32]}`,
    createdAt: now, name: `Device ${index}`, fw: '0.6.2',
  }));
  writeFileSync(pairing.path, JSON.stringify({ version: 1, approved: [], pending }));
  const saved = readFileSync(pairing.path, 'utf8');
  const writes = countPairingWrites(t, pairing.path);

  assert.deepEqual(await pairing.upsert('extra-device:0123456789abcdef0123456789abcdef'), { code: '', created: false });
  assert.equal(writes(), 0, 'queue capacity alone must not cause a disk write');
  assert.equal(readFileSync(pairing.path, 'utf8'), saved);
  assert.deepEqual((await pairing.list()).pending, pending);
});

test('a repeated pairing request still persists expiry without renewing its code or lifetime', async (t) => {
  const stateDir = stateDirectory(t);
  let now = 100_000;
  const codes = ['ABCDEFG2', 'ABCDEFG3'];
  const pairing = await createFilePairingStore(stateDir, { now: () => now, randomCode: () => codes.shift() }).ready();
  const expired = 'kubik-old:0123456789abcdef0123456789abcdef';
  const retained = 'kubik-current:0123456789abcdef0123456789abcdef';
  await pairing.upsert(expired);
  now += 1000;
  const request = await pairing.upsert(retained);
  const retainedAt = now;
  now += PAIRING_TTL_MS;
  const writes = countPairingWrites(t, pairing.path);

  assert.deepEqual(await pairing.upsert(retained), { code: request.code, created: false });
  assert.equal(writes(), 1, 'expiry must persist even when the requested code already exists');
  const stored = JSON.parse(readFileSync(pairing.path, 'utf8'));
  assert.deepEqual(stored.pending.map(({ entry, code, createdAt }) => ({ entry, code, createdAt })), [
    { entry: retained, code: request.code, createdAt: retainedAt },
  ]);
  const restarted = await createFilePairingStore(stateDir, { now: () => now }).ready();
  assert.deepEqual((await restarted.list()).pending, stored.pending);
  await assert.rejects(restarted.approve('ABCDEFG2'), /no unexpired pairing request/);
  assert.equal(writes(), 1, 'failed approval must not write the file');
});

test('public config and pairing reads repair private modes without changing saved state', async (t) => {
  const stateDir = stateDirectory(t);
  const expected = await writeHostConfig(stateDir, config(18790));
  const pairing = await createFilePairingStore(stateDir, { randomCode: () => 'ABCDEFG2' }).ready();
  await pairing.upsert('kubik-abcdef:0123456789abcdef0123456789abcdef', { name: 'Desk', fw: '0.6.2' });
  const expectedPairings = await pairing.list();

  chmodSync(stateDir, 0o755);
  chmodSync(join(stateDir, HOST_CONFIG_FILE), 0o644);
  chmodSync(join(stateDir, PAIRING_FILE), 0o644);

  assert.deepEqual(await readHostConfig(stateDir), expected);
  if (process.platform !== 'win32') {
    assert.equal(statSync(stateDir).mode & 0o777, 0o700);
    assert.equal(statSync(join(stateDir, HOST_CONFIG_FILE)).mode & 0o777, 0o600);
  }

  chmodSync(join(stateDir, PAIRING_FILE), 0o644);
  assert.deepEqual(await pairing.list(), expectedPairings);
  if (process.platform !== 'win32') assert.equal(statSync(join(stateDir, PAIRING_FILE)).mode & 0o777, 0o600);
});

test('public config and pairing paths reject state-directory and file symlinks without touching targets', async (t) => {
  if (process.platform === 'win32') return t.skip('POSIX no-follow flags are required');
  const root = stateDirectory(t);
  const targetDir = join(root, 'target-directory');
  mkdirSync(targetDir, { mode: 0o755 });
  chmodSync(targetDir, 0o755);
  const directoryLink = join(root, 'directory-link');
  symlinkSync(targetDir, directoryLink, 'dir');
  assert.throws(() => createFilePairingStore(directoryLink));
  await assert.rejects(readHostConfig(directoryLink));
  assert.equal(statSync(targetDir).mode & 0o777, 0o755);
  assert.deepEqual(readdirSync(targetDir), []);

  const stateDir = join(root, 'normal-state');
  mkdirSync(stateDir, { mode: 0o700 });
  await writeHostConfig(stateDir, config(18791));
  const configTarget = join(root, 'config-target.json');
  writeFileSync(configTarget, JSON.stringify(config(18792)), { mode: 0o644 });
  rmSync(join(stateDir, HOST_CONFIG_FILE));
  symlinkSync(configTarget, join(stateDir, HOST_CONFIG_FILE));
  await assert.rejects(readHostConfig(stateDir));
  assert.equal(statSync(configTarget).mode & 0o777, 0o644);
  assert.equal(readFileSync(configTarget, 'utf8'), JSON.stringify(config(18792)));

  const pairing = createFilePairingStore(stateDir);
  const pairingTarget = join(root, 'pairings-target.json');
  const pairings = JSON.stringify({ version: 1, approved: [], pending: [] });
  writeFileSync(pairingTarget, pairings, { mode: 0o644 });
  symlinkSync(pairingTarget, join(stateDir, PAIRING_FILE));
  await assert.rejects(pairing.allowed());
  assert.equal(statSync(pairingTarget).mode & 0o777, 0o644);
  assert.equal(readFileSync(pairingTarget, 'utf8'), pairings);
});

test('malformed config and pairing JSON errors do not expose file contents; missing config keeps ENOENT', async (t) => {
  const stateDir = stateDirectory(t);
  await assert.rejects(readHostConfig(stateDir), (error) => error.code === 'ENOENT');

  const secret = 'AGENT_STATE_DO_NOT_LEAK';
  writeFileSync(join(stateDir, HOST_CONFIG_FILE), `{"token":"${secret}"`);
  await assert.rejects(readHostConfig(stateDir), (error) => {
    assert.match(error.message, /invalid JSON/);
    assert.doesNotMatch(error.message, new RegExp(secret));
    return true;
  });

  writeFileSync(join(stateDir, PAIRING_FILE), `{"token":"${secret}"`);
  const pairing = createFilePairingStore(stateDir);
  await assert.rejects(pairing.list(), (error) => {
    assert.match(error.message, /invalid JSON/);
    assert.doesNotMatch(error.message, new RegExp(secret));
    return true;
  });
});

test('post-rename directory sync failure warns but keeps the committed config successful', async (t) => {
  if (process.platform === 'win32') return t.skip('directory fsync is unsupported on Windows');
  const stateDir = stateDirectory(t);
  await writeHostConfig(stateDir, config(18790));
  const warnings = [];
  const emitWarning = t.mock.method(process, 'emitWarning', (message, options) => {
    warnings.push({ message, options });
    throw new Error('injected warning handler failure');
  });
  const realFsync = fs.fsyncSync.bind(fs);
  const failDirectorySync = t.mock.method(fs, 'fsyncSync', (fd) => {
    if (fs.fstatSync(fd).isDirectory()) throw Object.assign(new Error('injected EIO'), { code: 'EIO' });
    return realFsync(fd);
  });
  syncBuiltinESMExports();
  try {
    await writeHostConfig(stateDir, config(18791));
    assert.equal((await readHostConfig(stateDir)).listener.port, 18791);
    assert.equal(warnings.length, 1);
    assert.equal(warnings[0].options.code, 'KUBIK_AGENT_STATE_DIRSYNC');
    assert.match(warnings[0].message, /committed but directory sync failed/);
    assert.deepEqual(readdirSync(stateDir), [HOST_CONFIG_FILE]);
  } finally {
    failDirectorySync.mock.restore();
    emitWarning.mock.restore();
    syncBuiltinESMExports();
  }
});
