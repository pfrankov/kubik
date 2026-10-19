import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';

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

// sim replies acknowledge enqueue; observe the actual result before the next action.
export async function waitForDeviceState(read, predicate, label, {timeoutMs = 5000, intervalMs = 40} = {}) {
  const deadline = Date.now() + timeoutMs;
  do {
    const state = await read();
    if (predicate(state)) return state;
    await sleep(intervalMs);
  } while (Date.now() < deadline);
  throw Error(`Device did not show ${label}`);
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

// Short contiguous windows avoid the display's periodic 10-second reset hiding earlier frames.
export function combineStats(windows) {
  assert.ok(windows.length && windows.every(complete), 'Complete frame windows required');
  const sum = key => windows.reduce((total, window) => total + window[key], 0);
  const frames = sum('frames'), elapsed_us = sum('elapsed_us');
  assert.ok(frames > 0 && elapsed_us > 0);
  const result = { frames, elapsed_us, fps: frames * 1e6 / elapsed_us,
    late: sum('late'), maxMs: Math.max(...windows.map(window => window.maxMs)),
    gapMs: Math.max(...windows.map(window => window.gapMs)),
    presented: sum('presented'), overflows: sum('overflows'), character: windows[0].character };
  for (const key of ['meanMs', 'face', 'shapes', 'prep', 'image', 'glass', 'hash', 'send']) {
    assert.ok(windows.every(window => Number.isFinite(window[key])), `Missing ${key} timing`);
    result[key] = windows.reduce((total, window) => total + window[key] * window.frames, 0) / frames;
  }
  assert.ok(windows.every(window => window.character === result.character));
  return result;
}

// One framed response owns the complete reset window; ordinary logs remain best effort.
export async function flushStats({ sim }) {
  const reply = await sim('stats');
  if (!reply?.ok || !Number.isInteger(reply.sequence) || !Number.isInteger(reply.modes) ||
      !Number.isInteger(reply.active) || !Number.isInteger(reply.presented) || !Number.isInteger(reply.overflows) ||
      !reply.frames || !reply.elapsed_us)
    throw new MissingStats('Incomplete USB frame diagnostics; this window is unmeasured');
  const stats = { ...reply, fps: reply.frames * 1e6 / reply.elapsed_us,
    meanMs: reply.mean_us / 1000, maxMs: reply.max_us / 1000, gapMs: reply.gap_us / 1000,
    character: reply.character, slow: [] };
  if (!complete(stats)) throw new MissingStats('Incomplete USB frame diagnostics; this window is unmeasured');
  return stats;
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

// A listener request precedes asynchronous ADC/RX startup (and teardown).
// Wait only for that transition; recording or idle reads fail immediately.
export async function waitForIdleCapture(read, options) {
  const state = await waitForDeviceState(async () => {
    const s = await read();
    assert.equal(s.mic_open, false); assert.equal(s.auto_recording, false);
    assert.equal(s.mic_idle_reads, 0);
    return s;
  }, s => s.wake_listening ? s.mic_enabled && s.mic_rx_enabled : !s.mic_enabled && !s.mic_rx_enabled,
  'idle capture hardware', options);
  assertIdleCapture(state);
  return state;
}
