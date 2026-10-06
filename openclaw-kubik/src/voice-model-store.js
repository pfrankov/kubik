import { join } from 'node:path';
import { ensurePrivateDirectory, readJsonFile, withFileLock, writeJsonAtomic } from './agent-sdk/storage.js';

const MAX_ENTRIES = 128, MAX_BYTES = 128 * 1024;
const SLOTS = ['stt', 'tts', 'realtime', 'live'];
const validModel = value => typeof value === 'string' && value.length <= 160 && /^[^/\s]+\/[^\s\p{C}]+$/u.test(value);

function validEntry(entry) {
  if (!Array.isArray(entry) || entry.length !== 2) throw new Error('Invalid voice model entry');
  const [key, choices] = entry;
  if (typeof key !== 'string' || key.length > 512 || !choices || typeof choices !== 'object' || Array.isArray(choices))
    throw new Error('Invalid voice model identity');
  for (const [slot, model] of Object.entries(choices))
    if (slot === 'mode' ? !['classic', 'realtime', 'live'].includes(model) : !SLOTS.includes(slot) || !validModel(model)) throw new Error('Invalid stored voice model');
  return key;
}

function validate(state) {
  if (state?.version !== 1 || !Array.isArray(state.entries) || state.entries.length > MAX_ENTRIES)
    throw new Error('Invalid voice model state');
  const keys = new Set();
  for (const entry of state.entries) {
    const key = validEntry(entry);
    if (keys.has(key)) throw new Error('Invalid duplicate voice identity');
    keys.add(key);
  }
  return new Map(state.entries);
}

/** Own plugin state: bounded, private and atomic. Never requires OpenClaw's trusted-plugin store. */
export function createVoiceModelStore(directory) {
  const path = join(directory, 'voice-models.json');
  function read() {
    try { return validate(readJsonFile(path, MAX_BYTES)); }
    catch (error) { if (error.code === 'ENOENT') return new Map(); throw error; }
  }
  return {
    lookup(key) { return read().get(key) ?? {}; },
    async select(key, slot, model) {
      ensurePrivateDirectory(directory);
      await withFileLock(path, () => {
        const entries = read();
        if (!entries.has(key) && entries.size >= MAX_ENTRIES) throw new Error('Voice model store is full');
        entries.set(key, { ...entries.get(key), [slot]: model });
        const state = { version: 1, entries: [...entries] };
        validate(state);
        writeJsonAtomic(path, state, MAX_BYTES);
      });
    },
  };
}
