import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { parseStats } from './frames.mjs';

export class MissingStats extends Error {}

export async function setDeviceScreen(read, sim, dark, { timeoutMs = 5000, intervalMs = 50 } = {}) {
  if ((await read()).screen_dark === dark) return;
  await sim('pwr');
  const deadline = Date.now() + timeoutMs;
  do {
    if ((await read()).screen_dark === dark) return;
    await sleep(intervalMs);
  } while (Date.now() < deadline);
  throw Error(`Device screen did not become ${dark ? 'dark' : 'awake'}`);
}

// A preceding radio/setup test may have just restored the endpoint and rebooted.
// Do not capture an offline transition as the state that later tests must restore.
export async function waitForDeviceLink(read, { timeoutMs = 45000, intervalMs = 500 } = {}) {
  const deadline = Date.now() + timeoutMs;
  do {
    const state = await read();
    if (state.ok && state.online && state.via !== 'none') return state;
    await sleep(intervalMs);
  } while (Date.now() < deadline);
  throw Error('Device did not establish an authenticated connection before the deadline');
}
const complete = (stats) => stats.character &&
  ['fps', 'frames', 'meanMs', 'maxMs', 'late', 'gapMs', 'face', 'send'].every((key) => Number.isFinite(stats[key]));

// Lossy USB log lines cannot be merged across reset windows to manufacture a valid sample.
export async function flushStats({ sim, logSize, logSince, timeoutMs = 5000, intervalMs = 40 }) {
  const from = logSize();
  await sim('stats');
  const deadline = Date.now() + timeoutMs;
  do {
    const block = logSince(from).split('\n').filter((line) => /main: (display|profile|present)/.test(line)).join('\n');
    const stats = parseStats(block);
    if (complete(stats)) return stats;
    await sleep(intervalMs);
  } while (Date.now() < deadline);
  throw new MissingStats('Incomplete USB frame diagnostics; this window is unmeasured');
}

// Only unavailable diagnostics allow one fresh window. Numeric failures are returned unchanged.
export async function measureWindow(measure, report = console.warn) {
  try { return await measure(); }
  catch (error) {
    if (!(error instanceof MissingStats)) throw error;
    report(`WARN  ${error.message}; measuring one fresh window`);
    return measure();
  }
}

// Passive Tess inference is local capture, never an open/uploading microphone.
export function assertIdleCapture(state) {
  assert.equal(state.mic_open, false);
  assert.equal(state.auto_recording, false);
  if (state.wake_listening) {
    assert.equal(state.character, 'Tess');
    assert.ok(state.online && state.stt_known && state.stt_available && !state.menu && !state.screen_dark);
    assert.equal(state.mic_enabled, true); assert.equal(state.mic_rx_enabled, true);
  } else {
    assert.equal(state.mic_enabled, false); assert.equal(state.mic_rx_enabled, false);
  }
  assert.equal(state.mic_idle_reads, 0);
}
