import assert from 'node:assert/strict';
import fs from 'node:fs';
import { chmodSync, mkdirSync, mkdtempSync, readdirSync, statSync, symlinkSync, writeFileSync, rmSync } from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { IDENTITY_FILE, loadTlsIdentity } from '../src/tls-identity.js';

function temporaryDirectory(t) {
  const path = mkdtempSync(join(tmpdir(), 'kubik-tls-identity-'));
  t.after(() => rmSync(path, { recursive: true, force: true }));
  return path;
}

test('repairs existing LAN identity permissions without changing the pinned key', (t) => {
  const root = temporaryDirectory(t);
  const dir = join(root, 'kubik');
  const first = loadTlsIdentity(dir);

  chmodSync(dir, 0o755);
  chmodSync(first.path, 0o644);
  const repaired = loadTlsIdentity(dir);

  assert.equal(repaired.spkiHash, first.spkiHash);
  if (process.platform !== 'win32') {
    assert.equal(statSync(dir).mode & 0o777, 0o700);
    assert.equal(statSync(first.path).mode & 0o777, 0o600);
  }
});

test('does not follow a symlink for the identity directory or key file', (t) => {
  if (process.platform === 'win32') return t.skip('POSIX no-follow flags are required');
  const root = temporaryDirectory(t);

  const targetDirectory = join(root, 'target-directory');
  mkdirSync(targetDirectory, { mode: 0o755 });
  chmodSync(targetDirectory, 0o755);
  const directoryLink = join(root, 'directory-link');
  symlinkSync(targetDirectory, directoryLink, 'dir');
  assert.throws(() => loadTlsIdentity(directoryLink));
  assert.equal(statSync(targetDirectory).mode & 0o777, 0o755);
  assert.deepEqual(readdirSync(targetDirectory), []);

  const targetIdentityDir = join(root, 'target-identity');
  const targetIdentity = loadTlsIdentity(targetIdentityDir);
  const fileLinkDir = join(root, 'file-link-directory');
  mkdirSync(fileLinkDir, { mode: 0o700 });
  symlinkSync(targetIdentity.path, join(fileLinkDir, IDENTITY_FILE));
  assert.throws(() => loadTlsIdentity(fileLinkDir));
  assert.equal(statSync(targetIdentity.path).mode & 0o777, 0o600);
  assert.equal(loadTlsIdentity(targetIdentityDir).spkiHash, targetIdentity.spkiHash);
});

test('an oversized identity is bounded, quarantined, and replaced under the existing recovery policy', (t) => {
  const root = temporaryDirectory(t);
  const dir = join(root, 'kubik');
  mkdirSync(dir, { mode: 0o700 });
  const path = join(dir, IDENTITY_FILE);
  writeFileSync(path, 'x'.repeat(16 * 1024 + 1), { mode: 0o600 });

  const identity = loadTlsIdentity(dir);

  assert.equal(identity.created, true);
  assert.ok(readdirSync(dir).some((name) => name.startsWith(`${IDENTITY_FILE}.corrupt-`)));
  if (process.platform !== 'win32') assert.equal(statSync(path).mode & 0o777, 0o600);
});

test('corrupt identity recovery does not log private file content', (t) => {
  const dir = temporaryDirectory(t);
  const secret = 'TOPSECRET';
  writeFileSync(join(dir, IDENTITY_FILE), secret, { mode: 0o600 });
  const logs = [];
  const identity = loadTlsIdentity(dir, { log: (line) => logs.push(line) });
  assert.equal(identity.created, true);
  assert.ok(logs.some((line) => line.includes('unusable')));
  assert.ok(logs.every((line) => !line.includes(secret)));
});

test('failed identity persistence removes its private temporary file and permits a clean retry', (t) => {
  const dir = temporaryDirectory(t);
  const fail = t.mock.method(fs, 'fsyncSync', () => { throw new Error('injected sync failure'); });
  syncBuiltinESMExports();
  try {
    assert.throws(() => loadTlsIdentity(dir), /injected sync failure/);
    assert.deepEqual(readdirSync(dir), []);
  } finally {
    fail.mock.restore();
    syncBuiltinESMExports();
  }
  const saved = loadTlsIdentity(dir);
  assert.equal(saved.created, true);
  assert.equal(loadTlsIdentity(dir).spkiHash, saved.spkiHash);
});
