import { isDeviceId, normalizeDeviceId } from '../config.js';

export const ACTIVITY_CATEGORIES = Object.freeze(['', 'thinking', 'tool', 'coding', 'web', 'deploy', 'build', 'concierge', 'compacting', 'stall']);
export const MAX_ACTIVITY_DEVICES = 128;
export const MAX_EVENT_TTL_MS = 60_000;
const DEFAULT_ACTIVITY_TTL_MS = 30_000;
const DEFAULT_CRON_TTL_MS = 60_000;
const EMPTY_CRON = Object.freeze({ running: 0, next: -1 });

function validTtl(ttlMs, fallback) {
  const ttl = ttlMs === undefined ? fallback : ttlMs;
  if (!Number.isSafeInteger(ttl) || ttl < 1000 || ttl > MAX_EVENT_TTL_MS) {
    throw new Error(`event TTL must be an integer from 1000 to ${MAX_EVENT_TTL_MS} ms`);
  }
  return ttl;
}

function validDeviceId(raw) {
  const id = normalizeDeviceId(raw);
  if (!isDeviceId(id)) throw new Error('activity device id is invalid');
  return id;
}

function validActivity(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)
    || Object.keys(value).some((key) => !['own', 'other'].includes(key))) {
    throw new Error('activity snapshot must contain own and/or other');
  }
  const result = { own: value.own ?? '', other: value.other ?? '' };
  if (!ACTIVITY_CATEGORIES.includes(result.own) || !ACTIVITY_CATEGORIES.includes(result.other)) {
    throw new Error('activity values must use a Kubik activity category');
  }
  return result;
}

function validCron(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)
    || Object.keys(value).some((key) => !['running', 'next'].includes(key))
    || !Number.isSafeInteger(value.running) || value.running < 0 || value.running > 65535
    || !Number.isSafeInteger(value.next) || value.next < -1 || value.next > 31_536_000) {
    throw new Error('cron snapshot needs running and next integer values');
  }
  return { running: value.running, next: value.next };
}

export function createActivityRelay({ onChange = () => {}, now = Date.now } = {}) {
  const activities = new Map();
  const timers = new Map();
  const sessionPrefix = 'kubik-agent:';
  const clearEntry = (id) => {
    const timer = timers.get(id);
    if (timer) clearTimeout(timer);
    timers.delete(id);
    return activities.delete(id);
  };
  const sessionKeyFor = (rawId) => `${sessionPrefix}${validDeviceId(rawId)}`;
  const idForSession = (key) => typeof key === 'string' && key.startsWith(sessionPrefix)
    ? normalizeDeviceId(key.slice(sessionPrefix.length)) : '';
  const summary = (sessionKey) => {
    const ownId = idForSession(sessionKey);
    const current = activities.get(ownId);
    let other = current?.other ?? '';
    let newest = current?.other ? current.updatedAt : -1;
    for (const [id, item] of activities) {
      if (id === ownId) continue;
      const value = item.own || item.other;
      if (value && item.updatedAt > newest) { other = value; newest = item.updatedAt; }
    }
    return { own: current?.own ?? '', other };
  };

  return {
    sessionKeyFor,
    summary,
    set(rawId, rawSnapshot, ttlMs) {
      const id = validDeviceId(rawId);
      const snapshot = validActivity(rawSnapshot);
      const ttl = validTtl(ttlMs, DEFAULT_ACTIVITY_TTL_MS);
      const nowAt = now();
      if (!activities.has(id) && activities.size >= MAX_ACTIVITY_DEVICES) {
        let oldestId = '';
        let oldestAt = Infinity;
        for (const [entryId, item] of activities) if (item.updatedAt < oldestAt) { oldestId = entryId; oldestAt = item.updatedAt; }
        if (oldestId) clearEntry(oldestId);
      }
      clearEntry(id);
      const token = Symbol(id);
      activities.set(id, { ...snapshot, updatedAt: nowAt, token });
      const timer = setTimeout(() => {
        if (activities.get(id)?.token !== token) return;
        clearEntry(id);
        onChange();
      }, ttl);
      timer.unref?.();
      timers.set(id, timer);
      onChange();
      return { deviceId: id, ...snapshot, expiresAt: nowAt + ttl };
    },
    close() {
      for (const timer of timers.values()) clearTimeout(timer);
      timers.clear();
      activities.clear();
    },
    get size() { return activities.size; },
  };
}

export function createCronRelay({ onChange = () => {}, now = Date.now } = {}) {
  let current = { ...EMPTY_CRON };
  let expiresAt = 0;
  let timer;
  let token = 0;
  const clear = (notify) => {
    if (timer) clearTimeout(timer);
    timer = null;
    expiresAt = 0;
    current = { ...EMPTY_CRON };
    if (notify) onChange();
  };
  return {
    summary() {
      if (expiresAt && expiresAt <= now()) clear(true);
      return { ...current };
    },
    set(value, ttlMs) {
      const next = validCron(value);
      const ttl = validTtl(ttlMs, DEFAULT_CRON_TTL_MS);
      current = next;
      expiresAt = now() + ttl;
      const mine = ++token;
      if (timer) clearTimeout(timer);
      timer = setTimeout(() => {
        if (token !== mine) return;
        clear(true);
      }, ttl);
      timer.unref?.();
      onChange();
      return { ...current };
    },
    close() { clear(false); },
  };
}
