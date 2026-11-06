#!/usr/bin/env node
// Real device admission recovery; local pairing/engine only, no provider calls or key changes.
// Bridge: ws://127.0.0.1:18999/kubik/v1, or kubik://127.0.0.1:18999 with the trusted TLS identity.
import assert from 'node:assert/strict';
import { createUsbListener } from './test-device/usb-listener.mjs';
import { assertSettingRestored, fixtureBaseline, restoreFixture, routeRestored } from './test-device/fixture-baseline.mjs';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';
import { KubikServer } from '../openclaw-kubik/src/server.js';
import { decodeDeviceKey } from '../openclaw-kubik/src/protocol.js';
import { account, fakeEngine } from '../openclaw-kubik/test/helpers.js';

const preserved = ['device', 'key', 'url', 'agent', 'ssid', 'volume', 'ui_volume', 'brightness', 'character', 'server_pinned', 'guide_done'];
async function control(command) {
  const response = await fetch('http://127.0.0.1:18791/config', {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify(command), signal: AbortSignal.timeout(5000),
  });
  assert.equal(response.status, 200);
  const state = await response.json();
  assert.equal(state.ok, true);
  return state;
}
const info = () => control({ cmd: 'info' });
const sim = (ev, extra = {}) => control({ cmd: 'sim', ev, ...extra });
async function until(check, label, timeoutMs = 40000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const state = await info();
    if (check(state)) return state;
    await sleep(100);
  }
  throw Error(`Timed out: ${label}`);
}
export async function restorePairingFixture(original, { simulate = sim, read = info, wait = until } = {}) {
  await simulate('wifi', { ms: 1 });
  await wait(state => routeRestored(state, original) && !state.pair_visible, 'original connection');
  if ((await read()).screen_dark !== original.screen_dark) await simulate('pwr');
  const current = await read();
  for (const key of preserved) assertSettingRestored(current, original, key);
  console.log('PASS original connection, paired identity and settings restored');
}
async function main() {
  const { original, offlineFixture } = await fixtureBaseline(info);
  assert.ok(offlineFixture || original.online && original.via === 'wifi', 'Start with the normal Wi-Fi agent or an explicit offline TLS USB fixture');
  assert.ok(!original.mic_open && !original.live_active && !original.menu && !original.screen_lab &&
    !original.card_open && !original.pair_visible, 'Device must be idle before admission test');
  const entry = `${original.device}:${decodeDeviceKey(original.key).fingerprint}`;
  let approved = false, requests = 0, attempts = 0;
  const logs = [];
  const pairing = {
    allowed: async () => approved ? [entry] : [],
    upsert: async identity => {
      assert.equal(identity, entry);
      requests++;
      return { code: 'TEST2345', created: true };
    },
  };
  const server = new KubikServer({ account: account(), pairing, pairingPollMs: 60000,
    dispatch: async () => {}, log: line => { if (logs.length < 64) logs.push(line); },
    engineFactory: () => {
      if (++attempts === 1) throw Error('LOCAL_PAIRING_STARTUP_FAILURE');
      const engine = fakeEngine();
      engine.canSpeak = false;
      return engine;
    } });
  const listener = createUsbListener({ allowPlain: original.url.startsWith('wss://') && !original.server_pinned });
  const { http, bind } = listener;
  http.on('upgrade', (req, socket, head) => server.handleUpgrade(req, socket, head,
    { via: 'usb', bind, remote: '127.0.0.1', hint: '' }));
  let entered = false;
  try {
    server.start();
    await listener.listen();
    entered = true; // A lost reply may still have applied the wake or bounded radio pause.
    if (original.screen_dark) await sim('pwr');
    await sim('wifi', { ms: 120000 });
    await until(state => state.pair_visible && requests === 1, 'pairing prompt on the real device');
    approved = true;
    await assert.doesNotReject(server.checkPairings());
    assert.ok(logs.some(line => line.includes('cannot start session for approved device') &&
      line.includes('LOCAL_PAIRING_STARTUP_FAILURE')), 'The approved admission really failed');
    await until(state => state.online && state.via === 'usb' && !state.pair_visible &&
      server.onlineDevices.includes(original.device), 'automatic authenticated retry without a new code');
    assert.equal(attempts, 2, 'One failed startup and one successful retry');
    assert.equal(requests, 1, 'Retry must reuse the approved key');
    console.log('PASS physical pairing → approved startup failure → automatic welcome without another pairing request');
  } finally {
    await restoreFixture({ offlineFixture, restore: async () => { if (entered) await restorePairingFixture(original); },
      close: async () => { try { await server.stop(); } finally { await listener.close(); } } });
  }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  main().catch(error => { console.error(error.message); process.exitCode = 1; });
