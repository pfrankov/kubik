import assert from 'node:assert/strict';
import fs from 'node:fs';
import { chmodSync, mkdtempSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync } from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { createFilePairingStore, PAIRING_FILE } from '../src/agent-sdk/pairing-store.js';
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
