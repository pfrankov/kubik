import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
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
  writeFileSync(path, '{oops');
  assert.throws(() => new NotificationQueue({ path }), SyntaxError);
  writeFileSync(path, JSON.stringify({ version: 7, entries: [] }));
  assert.throws(() => new NotificationQueue({ path }), /Invalid notification queue/);
  writeFileSync(path, ' '.repeat(512 * 1024 + 1));
  assert.throws(() => new NotificationQueue({ path }), /exceeds its limit/);
});
