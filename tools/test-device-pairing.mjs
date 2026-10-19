#!/usr/bin/env node
// Real device admission recovery; local pairing/engine only, no provider calls or key changes.
// Bridge: ws://127.0.0.1:18999/kubik/v1, or kubik://127.0.0.1:18999 with the trusted TLS identity.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createServer as createTlsServer } from 'node:https';
import { readFileSync } from 'node:fs';
import { setTimeout as sleep } from 'node:timers/promises';
import { KubikServer } from '../openclaw-kubik/src/server.js';
import { decodeDeviceKey, spkiSha256 } from '../openclaw-kubik/src/protocol.js';
import { account, fakeEngine } from '../openclaw-kubik/test/helpers.js';

const preserved = ['device', 'key', 'url', 'agent', 'ssid', 'volume', 'ui_volume', 'brightness', 'guide_done'];
function listener(original) {
  const path = process.env.KUBIK_TEST_TLS_IDENTITY;
  if (!path) {
    assert.ok(original.url.startsWith('wss://') && !original.server_pinned,
      'LAN devices require KUBIK_TEST_TLS_IDENTITY with their existing trusted key; bridge target kubik://127.0.0.1:18999');
    return { http: createServer(), bind: 'none' };
  }
  const { key, cert } = JSON.parse(readFileSync(path, 'utf8'));
  const http = createTlsServer({ key, cert, minVersion: 'TLSv1.2' });
  return { http, bind: spkiSha256(cert) };
}
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
async function restore(original) {
  await sim('wifi', { ms: 1 });
  await until(state => state.online === original.online && state.via === original.via &&
    state.wifi_connected === original.wifi_connected && !state.pair_visible, 'original connection');
  if ((await info()).screen_dark !== original.screen_dark) await sim('pwr');
  const current = await info();
  for (const key of preserved) assert.ok(current[key] === original[key], `Setting changed: ${key}`);
  console.log('PASS original connection, paired identity and settings restored');
}
async function main() {
  const original = await info();
  assert.ok(original.online && original.via === 'wifi', 'Start with the normal Wi-Fi agent connected');
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
  const { http, bind } = listener(original);
  http.on('upgrade', (req, socket, head) => server.handleUpgrade(req, socket, head,
    { via: 'usb', bind, remote: '127.0.0.1', hint: '' }));
  let entered = false;
  try {
    server.start();
    await new Promise((resolve, reject) => { http.once('error', reject); http.listen(18999, '127.0.0.1', resolve); });
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
    try { if (entered) await restore(original); }
    finally { try { await server.stop(); } finally { http.close(); } }
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
