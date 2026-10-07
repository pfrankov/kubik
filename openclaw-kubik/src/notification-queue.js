import { randomUUID } from 'node:crypto';
import { dirname } from 'node:path';
import { isDeviceId } from './config.js';
import { ensurePrivateDirectory, readPrivateFile, writePrivateFileAtomic } from './private-storage.js';

export const NOTIFICATION_LIMITS = Object.freeze({ perDevice: 8, total: 128, textBytes: 4096, totalBytes: 256 * 1024, ttlMs: 24 * 60 * 60_000 });
const MAX_FILE_BYTES = 512 * 1024;
const MAX_KEYS = 8;
const KEY_RE = /^[a-f0-9]{32}$/;

function notificationLimits(overrides) {
  const limits = { ...NOTIFICATION_LIMITS, ...overrides };
  for (const [key, value] of Object.entries(limits)) {
    if (!(key in NOTIFICATION_LIMITS) || !Number.isSafeInteger(value) || value < 1 || value > NOTIFICATION_LIMITS[key]) {
      throw new TypeError(`Invalid notification limit ${key}`);
    }
  }
  return limits;
}

function validKeys(keys) {
  return Array.isArray(keys) && keys.length > 0 && keys.length <= MAX_KEYS && keys.every((key) => KEY_RE.test(key));
}

function validLifetime(entry, ttlMs) {
  return Number.isSafeInteger(entry.createdAt) && entry.createdAt >= 0 && Number.isSafeInteger(entry.expiresAt)
    && entry.expiresAt > entry.createdAt && entry.expiresAt - entry.createdAt <= ttlMs;
}

export class NotificationError extends Error {
  constructor(message, { attempted = false, retryable = false } = {}) {
    super(message);
    this.name = 'NotificationError';
    this.attempted = attempted;
    this.retryable = retryable;
  }
}

/** Bounded FIFO, optionally persisted by atomic, fsynced replacement (one writer per path).
 * Admission never silently evicts a message. Keep entries until ACK, identity revocation or the documented 24-hour expiry; an unknown result is retried on reconnect.
 * An intentional user cancellation consumes the event. */
export class NotificationQueue {
  #entries = [];
  constructor({ path = null, now = Date.now, limits = {}, log = () => {} } = {}) {
    this.path = path;
    this.now = now;
    this.log = log;
    this.limits = notificationLimits(limits);
    if (path) this.#restore();
  }

  #restore() {
    ensurePrivateDirectory(dirname(this.path), 'Notification queue');
    const { text, tooLarge } = readPrivateFile(this.path, MAX_FILE_BYTES, 'Notification queue file');
    if (text === null) return;
    if (tooLarge) throw new Error('Notification queue file exceeds its limit');
    let data;
    try { data = JSON.parse(text); } catch { throw new Error('Invalid notification queue JSON'); }
    if (data?.version !== 1 || !Array.isArray(data.entries) || data.entries.length > NOTIFICATION_LIMITS.total) {
      throw new Error('Invalid notification queue file');
    }
    const ids = new Set();
    for (const entry of data.entries) {
      this.#validate(entry);
      if (ids.has(entry.id)) throw new Error('Duplicate notification queue id');
      ids.add(entry.id);
    }
    this.#entries = data.entries;
    this.prune();
    this.#checkCapacity(this.#entries);
  }

  get durable() { return Boolean(this.path); }
  get empty() { return this.#entries.length === 0; }
  get size() { this.prune(); return this.#entries.length; }
  has(id) { return this.#entries.some((entry) => entry.id === id && entry.expiresAt > this.now()); }

  #validate(entry) {
    if (!entry || typeof entry.id !== 'string' || !/^[a-f0-9-]{36}$/.test(entry.id) || !isDeviceId(entry.deviceId)
      || typeof entry.text !== 'string' || !entry.text.trim() || Buffer.byteLength(entry.text) > this.limits.textBytes
      || !validKeys(entry.keys) || !validLifetime(entry, this.limits.ttlMs)) {
      throw new NotificationError('Invalid notification or message exceeds queue limits');
    }
  }

  #checkCapacity(entries) {
    const counts = new Map();
    let bytes = 0;
    for (const entry of entries) {
      counts.set(entry.deviceId, (counts.get(entry.deviceId) ?? 0) + 1);
      bytes += Buffer.byteLength(entry.text);
    }
    if (entries.length > this.limits.total || bytes > this.limits.totalBytes
      || [...counts.values()].some((count) => count > this.limits.perDevice)) {
      throw new NotificationError('Notification queue is full; the message was not accepted', { retryable: true });
    }
  }

  #commit(entries) {
    let directorySyncError;
    if (this.path) {
      const dir = dirname(this.path);
      ensurePrivateDirectory(dir, 'Notification queue');
      directorySyncError = writePrivateFileAtomic(this.path, JSON.stringify({ version: 1, entries }));
    }
    this.#entries = entries;
    if (directorySyncError) {
      try { this.log(`kubik: notification queue committed but its directory could not be synced (${directorySyncError.code ?? 'I/O error'})`); }
      catch { /* the file rename already committed this queue state */ }
    }
  }

  prune() {
    const alive = this.#entries.filter((entry) => entry.expiresAt > this.now());
    if (alive.length !== this.#entries.length) this.#commit(alive);
  }

  enqueue(deviceId, text, keys) {
    this.prune();
    const createdAt = this.now();
    const entry = { id: randomUUID(), deviceId, text, keys: [...new Set(keys)], createdAt, expiresAt: createdAt + this.limits.ttlMs };
    this.#validate(entry);
    const entries = [...this.#entries, entry];
    this.#checkCapacity(entries);
    this.#commit(entries);
    return { ...entry, keys: [...entry.keys] };
  }

  peek(deviceId, key) {
    this.prune();
    const entry = this.#entries.find((value) => value.deviceId === deviceId && value.keys.includes(key));
    return entry && { ...entry, keys: [...entry.keys] };
  }

  remove(id) {
    const entries = this.#entries.filter((entry) => entry.id !== id);
    if (entries.length !== this.#entries.length) this.#commit(entries);
  }

  /** Drop revoked identities. A later re-pairing cannot recover messages addressed to an old key. */
  revoke(allowed) {
    const entries = [], now = this.now();
    let changed = false;
    for (const entry of this.#entries) {
      if (entry.expiresAt <= now) { changed = true; continue; }
      const keys = entry.keys.filter((key) => allowed.has(`${entry.deviceId}:${key}`));
      if (keys.length === entry.keys.length) entries.push(entry);
      else {
        changed = true;
        if (keys.length) entries.push({ ...entry, keys });
      }
    }
    if (changed) this.#commit(entries);
  }
}
