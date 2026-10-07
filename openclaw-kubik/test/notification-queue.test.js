import assert from 'node:assert/strict';
import fs from 'node:fs';
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync } from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { NotificationQueue, NOTIFICATION_LIMITS } from '../src/notification-queue.js';
import { DEFAULT_DEVICE_KEY, DEVICE } from './helpers.js';

const keys = [DEFAULT_DEVICE_KEY.fingerprint];
const pathFor = (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-notification-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return join(dir, 'notifications.json');
};

test('durable queue preserves FIFO across restart and consumes entries before replay', (t) => {
  const path = pathFor(t);
  const queue = new NotificationQueue({ path });
  const first = queue.enqueue(DEVICE, 'Первое', keys);
  const second = queue.enqueue(DEVICE, 'Второе', keys);
  assert.equal(statSync(path).mode & 0o777, 0o600);
  const restored = new NotificationQueue({ path });
  assert.equal(restored.durable, true);
  assert.equal(restored.peek(DEVICE, keys[0]).id, first.id);
  restored.remove(first.id);
  const restarted = new NotificationQueue({ path });
  assert.equal(restarted.peek(DEVICE, keys[0]).id, second.id);
  restarted.remove(second.id);
  assert.equal(new NotificationQueue({ path }).size, 0);
});

test('restore repairs directory and queue-file permissions without changing queued content', (t) => {
  const path = pathFor(t);
  const original = new NotificationQueue({ path }).enqueue(DEVICE, 'Личное напоминание', keys);
  chmodSync(dirname(path), 0o755);
  chmodSync(path, 0o644);

  const restored = new NotificationQueue({ path });

  assert.equal(restored.peek(DEVICE, keys[0]).id, original.id);
  if (process.platform !== 'win32') {
    assert.equal(statSync(dirname(path)).mode & 0o777, 0o700);
    assert.equal(statSync(path).mode & 0o777, 0o600);
  }
});

test('expiration frees capacity and is persisted; expired content never replays', (t) => {
  const path = pathFor(t);
  let now = 1000;
  const queue = new NotificationQueue({ path, now: () => now, limits: { ttlMs: 100, perDevice: 1 } });
  queue.enqueue(DEVICE, 'Старое', keys);
  now = 1100;
  assert.equal(queue.peek(DEVICE, keys[0]), undefined);
  assert.equal(JSON.parse(readFileSync(path, 'utf8')).entries.length, 0);
  queue.enqueue(DEVICE, 'Новое', keys);
  now = 1200;
  assert.equal(new NotificationQueue({ path, now: () => now }).size, 0);
});

test('per-device, total, UTF-8 text and total byte limits reject without evicting accepted messages', () => {
  const perDevice = new NotificationQueue({ limits: { perDevice: 1 } });
  const first = perDevice.enqueue(DEVICE, 'Первое', keys);
  assert.throws(() => perDevice.enqueue(DEVICE, 'Второе', keys), /queue is full/);
  assert.equal(perDevice.peek(DEVICE, keys[0]).id, first.id);
  const total = new NotificationQueue({ limits: { total: 1 } });
  total.enqueue(DEVICE, 'Первое', keys);
  assert.throws(() => total.enqueue('kubik-abcdef', 'Второе', keys), /queue is full/);
  const bytes = new NotificationQueue({ limits: { totalBytes: 4 } });
  bytes.enqueue(DEVICE, 'Да', keys);
  assert.throws(() => bytes.enqueue('kubik-abcdef', 'a', keys), /queue is full/);
  assert.throws(() => total.enqueue(DEVICE, 'ы'.repeat(NOTIFICATION_LIMITS.textBytes / 2 + 1), keys), /exceeds queue limits/);
  assert.throws(() => new NotificationQueue({ limits: { total: NOTIFICATION_LIMITS.total + 1 } }), /Invalid notification limit/);
});

test('revocation purges old content, multiple approved keys retain only authorized identities', (t) => {
  const path = pathFor(t);
  const other = 'b'.repeat(32);
  let now = 1000;
  const queue = new NotificationQueue({ path, now: () => now, limits: { ttlMs: 100 } });
  queue.enqueue(DEVICE, 'Личное', [...keys, other]);
  const original = statSync(path, { bigint: true });
  queue.revoke(new Set([`${DEVICE}:${keys[0]}`, `${DEVICE}:${other}`]));
  assert.equal(statSync(path, { bigint: true }).ino, original.ino, 'No-op revocation must not replace the persisted file');
  queue.revoke(new Set([`${DEVICE}:${other}`]));
  assert.equal(queue.peek(DEVICE, keys[0]), undefined);
  assert.equal(queue.peek(DEVICE, other).text, 'Личное');
  now += 100;
  queue.revoke(new Set([`${DEVICE}:${other}`]));
  assert.equal(JSON.parse(readFileSync(path, 'utf8')).entries.length, 0, 'Revocation pass also expires authorized entries');
  queue.enqueue(DEVICE, 'Новое', keys);
  queue.revoke(new Set());
  assert.equal(new NotificationQueue({ path }).size, 0);
});

test('malformed or oversized durable state fails closed, with no silent in-memory fallback', (t) => {
  const path = pathFor(t);
  const secret = 'TOPSECRET_NOTIFICATION_CONTENT';
  writeFileSync(path, secret);
  assert.throws(() => new NotificationQueue({ path }), (error) => {
    assert.match(error.message, /Invalid notification queue JSON/);
    assert.doesNotMatch(error.message, /TOPSECRET_NOTIFICATION_CONTENT/);
    return true;
  });
  writeFileSync(path, '{"version":1,"entries":[]}');
  writeFileSync(path, JSON.stringify({ version: 7, entries: [] }));
  assert.throws(() => new NotificationQueue({ path }), /Invalid notification queue/);
  writeFileSync(path, ' '.repeat(512 * 1024 + 1));
  assert.throws(() => new NotificationQueue({ path }), /exceeds its limit/);
});

test('restore refuses queue symlinks without changing the target file', (t) => {
  if (process.platform === 'win32') return t.skip('POSIX no-follow flags are required');
  const root = mkdtempSync(join(tmpdir(), 'kubik-notification-links-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const queueDir = join(root, 'queue');
  const target = join(root, 'target.json');
  const contents = JSON.stringify({ version: 1, entries: [] });
  mkdirSync(queueDir, { mode: 0o700 });
  writeFileSync(target, contents, { mode: 0o644 });
  symlinkSync(target, join(queueDir, 'notifications.json'));

  assert.throws(() => new NotificationQueue({ path: join(queueDir, 'notifications.json') }));
  assert.equal(readFileSync(target, 'utf8'), contents);
  assert.equal(statSync(target).mode & 0o777, 0o644, 'restore must not chmod the symlink target');
});

test('failed queue persistence removes its unique temporary file and retains the prior queue', (t) => {
  const path = pathFor(t);
  const queue = new NotificationQueue({ path });
  const first = queue.enqueue(DEVICE, 'Сохранённое сообщение', keys);
  const before = readFileSync(path, 'utf8');
  const fail = t.mock.method(fs, 'fsyncSync', () => { throw new Error('injected sync failure'); });
  syncBuiltinESMExports();
  try {
    assert.throws(() => queue.enqueue(DEVICE, 'Не должно сохраниться', keys), /injected sync failure/);
    assert.equal(readFileSync(path, 'utf8'), before, 'pre-rename failure preserves the old queue file');
    assert.deepEqual(readdirSync(dirname(path)), ['notifications.json']);
    assert.equal(queue.peek(DEVICE, keys[0]).id, first.id, 'memory changes only after persistence succeeds');
  } finally {
    fail.mock.restore();
    syncBuiltinESMExports();
  }
});

test('post-rename directory sync failures do not report a committed notification as failed', (t) => {
  if (process.platform === 'win32') return t.skip('directory fsync is unsupported on Windows');
  for (const [code, warning] of [['EIO', true], ['EINVAL', false]]) {
    const path = pathFor(t);
    const logs = [];
    const queue = new NotificationQueue({ path, log: (line) => logs.push(line) });
    let syncs = 0;
    const realFsync = fs.fsyncSync.bind(fs);
    const failDirectorySync = t.mock.method(fs, 'fsyncSync', (fd) => {
      if (++syncs === 2) throw Object.assign(new Error('injected directory sync failure'), { code });
      return realFsync(fd);
    });
    syncBuiltinESMExports();
    try {
      const entry = queue.enqueue(DEVICE, 'Доставить после переименования', keys);
      assert.equal(queue.peek(DEVICE, keys[0]).id, entry.id);
      assert.equal(JSON.parse(readFileSync(path, 'utf8')).entries[0].id, entry.id);
      assert.deepEqual(readdirSync(dirname(path)), ['notifications.json']);
      assert.equal(logs.some((line) => line.includes('EIO')), warning);
    } finally {
      failDirectorySync.mock.restore();
      syncBuiltinESMExports();
    }
  }
});
