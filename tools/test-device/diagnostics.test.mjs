import assert from 'node:assert/strict';
import test from 'node:test';
import { flushStats, measureWindow, MissingStats, setDeviceScreen, waitForDeviceLink } from './diagnostics.mjs';

test('screen preflight wakes only a dark display, restores it and rejects a missed power event', async () => {
  let dark = true, toggles = 0;
  const read = async () => ({ screen_dark: dark });
  const sim = async (event) => { assert.equal(event, 'pwr'); toggles++; dark = !dark; };
  await setDeviceScreen(read, sim, false);
  await setDeviceScreen(read, sim, false);
  assert.equal(toggles, 1);
  await setDeviceScreen(read, sim, true);
  assert.equal(dark, true);
  await assert.rejects(setDeviceScreen(read, async () => {}, false, { timeoutMs: 5, intervalMs: 1 }), /did not become awake/);
});

test('device preflight waits for confirmed reconnect and rejects a persistently offline or broken bridge', async () => {
  let calls = 0;
  const read = async () => ({ok: true, online: ++calls >= 3, via: calls >= 3 ? 'wifi' : 'none'});
  const state = await waitForDeviceLink(read, {timeoutMs: 100, intervalMs: 1});
  assert.equal(calls, 3); assert.equal(state.online, true);
  await assert.rejects(waitForDeviceLink(async () => ({ok: true, online: false, via: 'none'}),
    {timeoutMs: 5, intervalMs: 1}), /authenticated connection/);
  await assert.rejects(waitForDeviceLink(async () => { throw Error('USB unavailable'); }), /USB unavailable/);
});

const block = 'main: display: 30.0 fps (30 frames), 20000 us/frame (max 31000, 0 over 33.3 ms)\n' +
  'main: profile us/frame: face 3000, render+send 17000 (prep 1, image 1, shapes 1, glass 1, hash 1, pack 1, send 1)\n' +
  'main: present: frames shown 20000..35000 us apart, character Tess';

test('stats require a complete fresh sample, not old or partial reset windows', async () => {
  let text = 'old';
  const options = { logSize: () => text.length, logSince: (from) => text.slice(from),
    sim: async () => { text += block; }, timeoutMs: 5, intervalMs: 1 };
  assert.equal((await flushStats(options)).frames, 30);
  await assert.rejects(flushStats({ ...options, sim: async () => { text += 'main: present: character Tess'; } }), MissingStats);
});

test('one missing observation is retried; numeric and other failures are never retried', async () => {
  let calls = 0;
  const result = await measureWindow(async () => { if (++calls === 1) throw new MissingStats('lost'); return { late: 99 }; }, () => {});
  assert.equal(calls, 2); assert.equal(result.late, 99);
  calls = 0;
  await assert.rejects(measureWindow(async () => { calls++; throw new MissingStats('lost'); }, () => {}), MissingStats);
  assert.equal(calls, 2);
  calls = 0;
  await assert.rejects(measureWindow(async () => { calls++; throw Error('device rejected'); }), /device rejected/);
  assert.equal(calls, 1);
});

import { assertIdleCapture } from './diagnostics.mjs';
test('idle capture distinguishes passive Tess from blocked states and Plush', () => {
  const off = {mic_open:false,auto_recording:false,mic_enabled:false,mic_rx_enabled:false,mic_idle_reads:0};
  assertIdleCapture({...off, character:'Plush'});
  const tess = {...off,character:'Tess',wake_listening:true,online:true,stt_known:true,stt_available:true,menu:false,screen_dark:false,mic_enabled:true,mic_rx_enabled:true};
  assertIdleCapture(tess);
  for (const patch of [{character:'Plush'}, {menu:true}, {screen_dark:true}, {online:false}, {stt_available:false}, {mic_open:true}, {mic_rx_enabled:false}])
    assert.throws(() => assertIdleCapture({...tess,...patch}));
  assert.throws(() => assertIdleCapture({...off,mic_enabled:true}));
});
