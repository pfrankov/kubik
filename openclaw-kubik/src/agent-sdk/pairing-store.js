import { randomBytes } from 'node:crypto';
import { join } from 'node:path';
import { isDeviceId, normalizeDeviceId } from '../config.js';
import { ensurePrivateDirectory, readJsonFile, withFileLock, writeJsonAtomic } from './storage.js';

export const PAIRING_FILE = 'pairings.json';
export const MAX_PENDING_PAIRINGS = 128;
export const MAX_APPROVED_KEYS = 512;
export const PAIRING_TTL_MS = 24 * 60 * 60 * 1000;
const MAX_FILE_BYTES = 64 * 1024;
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // exactly 32 unambiguous symbols
const ENTRY = /^([a-z0-9][a-z0-9_-]{0,63}):([a-f0-9]{32})$/;
const emptyState = () => ({ version: 1, approved: [], pending: [] });

function validateEntry(value) {
  if (typeof value !== 'string') throw new Error('invalid device key entry');
  const entry = value.trim().toLowerCase();
  const match = ENTRY.exec(entry);
  if (!match || !isDeviceId(normalizeDeviceId(match[1]))) throw new Error('invalid device key entry');
  return entry;
}

function validateStateShape(value) {
  if (!value || value.version !== 1) throw new Error('pairing state has an unsupported format');
  if (!Array.isArray(value.approved) || !Array.isArray(value.pending)) throw new Error('pairing state has an unsupported format');
  if (value.approved.length > MAX_APPROVED_KEYS || value.pending.length > MAX_PENDING_PAIRINGS) {
    throw new Error('pairing state has an unsupported or oversized format');
  }
}

function validateApproved(values) { return [...new Set(values.map(validateEntry))]; }

function validatePendingItem(item, approved, codes) {
  if (!item || typeof item !== 'object' || Array.isArray(item)) throw new Error('pairing state contains an invalid request');
  const entry = validateEntry(item.entry);
  const code = String(item.code ?? '').toUpperCase();
  const createdAt = Number(item.createdAt);
  if (!/^[A-HJ-NP-Z2-9]{8}$/.test(code)) throw new Error('pairing state contains an invalid request');
  if (!Number.isSafeInteger(createdAt) || createdAt < 0) throw new Error('pairing state contains an invalid request');
  validatePendingLabels(item);
  if (codes.has(code) || approved.includes(entry)) throw new Error('pairing state contains an invalid request');
  codes.add(code);
  return { entry, code, createdAt, name: item.name, fw: item.fw };
}

function validatePendingLabels(item) {
  if (typeof item.name !== 'string') throw new Error('pairing state contains an invalid request');
  if (item.name.length > 64) throw new Error('pairing state contains an invalid request');
  if (typeof item.fw !== 'string') throw new Error('pairing state contains an invalid request');
  if (item.fw.length > 32) throw new Error('pairing state contains an invalid request');
}

function validatePending(values, approved) {
  const pending = [];
  const codes = new Set();
  for (const item of values) pending.push(validatePendingItem(item, approved, codes));
  return pending;
}

function validateState(value) {
  validateStateShape(value);
  const approved = validateApproved(value.approved);
  const pending = validatePending(value.pending, approved);
  return { version: 1, approved, pending };
}

function readState(path) {
  try { return validateState(readJsonFile(path, MAX_FILE_BYTES)); }
  catch (error) {
    if (error.code === 'ENOENT') return emptyState();
    throw error;
  }
}

function makeCode(existing) {
  for (let attempt = 0; attempt < 8; attempt++) {
    const code = [...randomBytes(8)].map((value) => CODE_ALPHABET[value & 31]).join('');
    if (!existing.has(code)) return code;
  }
  throw new Error('could not generate a unique pairing code');
}

/**
 * Persistent pairing store compatible with KubikServer's `upsert`/`allowed` interface. Approval is only possible
 * through the local CLI command and a matching displayed code; connecting a device never approves its own key.
 */
export function createFilePairingStore(stateDir, { now = Date.now, randomCode = makeCode } = {}) {
  ensurePrivateDirectory(stateDir);
  const path = join(stateDir, PAIRING_FILE);

  async function mutate(update) {
    return withFileLock(path, async () => {
      const state = readState(path);
      const previous = JSON.stringify(state);
      const result = await update(state);
      const next = validateState(state);
      // Retries keep their code; persist only a changed request, approval or expiry.
      if (JSON.stringify(next) !== previous) writeJsonAtomic(path, next, MAX_FILE_BYTES);
      return result;
    });
  }

  return {
    path,
    async ready() {
      await withFileLock(path, async () => {
        try { validateState(readJsonFile(path, MAX_FILE_BYTES)); }
        catch (error) {
          if (error.code !== 'ENOENT') throw error;
          writeJsonAtomic(path, emptyState(), MAX_FILE_BYTES);
        }
      });
      return this;
    },
    async upsert(rawEntry, meta = {}) {
      const entry = validateEntry(rawEntry);
      const id = entry.slice(0, entry.indexOf(':'));
      const name = String(meta.name ?? id).slice(0, 64);
      const fw = String(meta.fw ?? '').slice(0, 32);
      return mutate((state) => {
        const cutoff = now() - PAIRING_TTL_MS;
        state.pending = state.pending.filter((item) => item.createdAt >= cutoff);
        const existing = state.pending.find((item) => item.entry === entry);
        if (state.approved.includes(entry)) return { code: '', created: false };
        if (existing) return { code: existing.code, created: false };
        if (state.pending.length >= MAX_PENDING_PAIRINGS) return { code: '', created: false };
        const code = randomCode(new Set(state.pending.map((item) => item.code)));
        if (!/^[A-HJ-NP-Z2-9]{8}$/.test(code)) throw new Error('pairing code generator returned an invalid code');
        state.pending.push({ entry, code, createdAt: now(), name, fw });
        return { code, created: true };
      });
    },
    async allowed() {
      return readState(path).approved;
    },
    async list() {
      const state = readState(path);
      const cutoff = now() - PAIRING_TTL_MS;
      return {
        approved: [...state.approved],
        pending: state.pending.filter((item) => item.createdAt >= cutoff).map((item) => ({ ...item })),
      };
    },
    async approve(rawCode) {
      const code = String(rawCode ?? '').trim().toUpperCase();
      if (!/^[A-HJ-NP-Z2-9]{8}$/.test(code)) throw new Error('pairing code must be eight letters or digits');
      return mutate((state) => {
        const cutoff = now() - PAIRING_TTL_MS;
        state.pending = state.pending.filter((item) => item.createdAt >= cutoff);
        const index = state.pending.findIndex((item) => item.code === code);
        if (index < 0) throw new Error('no unexpired pairing request has that code');
        const [request] = state.pending.splice(index, 1);
        if (state.approved.length >= MAX_APPROVED_KEYS && !state.approved.includes(request.entry)) {
          throw new Error('approved device-key store is full');
        }
        if (!state.approved.includes(request.entry)) state.approved.push(request.entry);
        return { entry: request.entry, deviceId: request.entry.slice(0, request.entry.indexOf(':')), name: request.name };
      });
    },
  };
}

export { validateEntry as validatePairingEntry };
