#!/usr/bin/env node
// Native TLS/Wi-Fi radio sleep, durable notification and <=60 s wake. Local mock, no paid APIs.
// Requires KUBIK_TEST_LAN_HOST=<computer IPv4 reachable from the saved Wi-Fi> and USB control.
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { isIP } from 'node:net';
import { join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';
import { KubikServer } from '../openclaw-kubik/src/server.js';
import { LanListener } from '../openclaw-kubik/src/lan.js';
import { loadTlsIdentity } from '../openclaw-kubik/src/tls-identity.js';
import { decodeDeviceKey } from '../openclaw-kubik/src/protocol.js';
import { account, fakeEngine } from '../openclaw-kubik/test/helpers.js';
import { startRadioRelay } from './radio-relay.mjs';
import { assertIdleCapture } from './test-device/diagnostics.mjs';

const host = process.env.KUBIK_TEST_LAN_HOST;
const controlUrl = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
let stopping = false, restoring = false;
const requestStop = () => { stopping = true; };
async function control(body) {
  const response = await fetch(`${controlUrl}/config`, { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body), signal: AbortSignal.timeout(5000) });
  assert.equal(response.status, 200, `USB control ${body.cmd}${body.ev ? `/${body.ev}` : ''}: HTTP ${response.status}`);
  const data = await response.json(); assert.equal(data.ok, true); return data;
}
const info = () => control({ cmd: 'info' });
const sim = (ev, extra = {}) => control({ cmd: 'sim', ev, ...extra });
async function until(check, label, timeout = 45000) {
  const end = performance.now() + timeout;
  while (performance.now() < end) {
    if (stopping && !restoring) throw Error('Radio test interrupted; restoring saved settings');
    const state = await info().catch(() => null); // re-enumeration during explicit server-setting reboot
    if (state && await check(state)) return state;
    await sleep(200);
  }
  throw Error(`Timed out: ${label}`);
}
async function darken() {
  await control({ cmd: 'card', text: '' });
  await sleep(300); // CONFIG reply acknowledges queuing; let the UI apply its clear before the power event
  const state = await info();
  if (state.menu) await sim('menu');
  if (!state.screen_dark) await sim('pwr');
  await until((s) => s.screen_dark, 'dark screen');
}
const queue = (path) => JSON.parse(readFileSync(path, 'utf8')).entries;
const deliveries = [];
const observedSessions = new WeakSet();
function observeDelivery(server, device) {
  const session = server.getSession(device);
  if (!session || observedSessions.has(session)) return;
  observedSessions.add(session);
  const notify = session.notify.bind(session);
  session.notify = async (...args) => {
    const result = await notify(...args); deliveries.push(result.status); return result;
  };
}
async function setEndpoint(url, extra = {}) {
  try { await control({ cmd: 'set', url, ...extra }); }
  catch (error) {
    // Saving may succeed while the explicit reboot drops its USB reply. Prove the saved outcome; never resend blindly.
    await until((s) => s.url === url, 'saved endpoint after lost reboot reply', 15000).catch(() => { throw error; });
  }
}
async function radioOff() {
  await darken();
  await sim('power-network', { ms: 120000 });
  return until((s) => s.network_phase === 1 && !s.wifi_radio_started && s.cpu_light_sleep, 'radio off and real CPU sleep allowed');
}
async function enqueueWhileOff(server, device, text, path) {
  try { assert.equal((await server.notify(device, text)).status, 'queued'); }
  catch (error) {
    // A TCP peer can remain apparently online after the radio stops. No ACK must retain the accepted event.
    assert.equal(error.name, 'NotificationError'); assert.equal(error.attempted, true);
  }
  assert.equal(queue(path).length, 1);
}
async function wakeWithinMinute(server, original, path, label) {
  const paused = await radioOff();
  const deliveredBefore = deliveries.length;
  const started = performance.now();
  await enqueueWhileOff(server, original.device, `Local radio wake ${label}`, path);
  const awake = await until((s) => !s.screen_dark && s.wifi_radio_started && s.via === 'wifi' &&
    queue(path).length === 0, `${label}: native Wi-Fi text ACK wakes screen`, 60000);
  const latency = performance.now() - started;
  assert.ok(latency <= 60000, `${label}: delivery took ${latency} ms`);
  assert.ok(awake.network_transitions > paused.network_transitions);
  assert.equal(awake.cpu_light_sleep, false, 'network delivery keeps CPU awake');
  assert.equal(awake.mic_enabled, false); assert.equal(awake.mic_rx_enabled, false);
  assert.equal(deliveries[deliveredBefore], 'shown', 'wake must confirm text receipt, not user interruption');
  console.log(`PASS ${label}: actual driver off → native TLS reconnect → text ACK/wake in ${(latency / 1000).toFixed(2)} s`);
}
async function unavailableServer(server, listener, original, path) {
  await radioOff();
  await listener.stop();
  await enqueueWhileOff(server, original.device, 'Retained during unavailable local server', path);
  await until((s) => s.network_phase === 2 && s.wifi_radio_started && !s.cpu_light_sleep, 'awake CPU during next radio check', 30000);
  await until((s) => s.network_phase === 1 && !s.wifi_radio_started, 'bounded unsuccessful check ends', 50000);
  assert.equal(queue(path).length, 1, 'failure cannot consume notification');
  const deliveredBefore = deliveries.length;
  await listener.start();
  await until((s) => !s.screen_dark && s.via === 'wifi' && queue(path).length === 0,
    'retained event delivered after server recovery', 60000);
  assert.equal(deliveries[deliveredBefore], 'shown');
  console.log('PASS unavailable server: bounded check turns radio off, durable entry survives and wakes on recovery');
}
async function restore(original) {
  await sim('power-network', { ms: 1 });
  await setEndpoint(original.url, { ssid: original.ssid, pass: '' });
  const state = await until((s) => s.url === original.url, 'original endpoint restored', 15000);
  for (const key of ['ssid', 'url', 'character', 'volume', 'brightness', 'guide_done']) assert.equal(state[key], original[key]);
  assert.equal(state.server_pinned, original.server_pinned);
  if (state.menu !== original.menu) await sim('menu');
  if (state.screen_dark !== original.screen_dark) await sim('pwr');
  await until((s) => s.online && s.via === original.via && s.key === original.key, 'original authenticated route and identity restored', 60000);
}
async function main() {
  assert.equal(isIP(host), 4, 'Set KUBIK_TEST_LAN_HOST to the computer IPv4 on the device Wi-Fi');
  const original = await info();
  assert.ok(original.ssid, 'Configure a saved Wi-Fi profile before the native radio test');
  assert.equal(original.online, true); assertIdleCapture(original);
  assert.equal(original.server_pinned, false, 'Use a provisioned CA-route device; this test must not erase an existing LAN pin');
  assert.match(original.url, /^wss:\/\//);
  const dir = mkdtempSync(join(tmpdir(), 'kubik-radio-'));
  let changed = false, server, listener, relay;
  process.on('SIGINT', requestStop); process.on('SIGTERM', requestStop);
  try {
    const path = join(dir, 'notifications.json');
    const identity = `${original.device}:${decodeDeviceKey(original.key).fingerprint}`;
    const engine = fakeEngine(); engine.canSpeak = false;
    server = new KubikServer({ account: account(), notificationPath: path, engineFactory: () => engine,
      pairing: { allowed: async () => [identity] }, dispatch: async () => {},
      log: (message) => { if (message.includes(' connected from ')) observeDelivery(server, original.device); } });
    listener = new LanListener({ identity: loadTlsIdentity(dir), port: 0, host,
      discoveryPort: 0, discoveryHost: host,
      onUpgrade: (req, socket, head, transport) => server.handleUpgrade(req, socket, head, transport) });
    server.start();
    await listener.start();
    listener.configuredPort = listener.port; // recovery reuses the allocated port already saved on the device
    relay = await startRadioRelay(host, listener.port);
    if (stopping) throw Error('Radio test interrupted before settings change');
    changed = true; // a lost config response may still have saved and rebooted
    await setEndpoint(relay?.url ?? `kubik://${host}:${listener.port}`);
    await until((s) => s.online && s.via === 'wifi' && server.onlineDevices.includes(original.device), 'native mock authenticated');
    await wakeWithinMinute(server, original, path, 'first cycle');
    await wakeWithinMinute(server, original, path, 'repeated cycle');
    await unavailableServer(server, listener, original, path);
  } catch (error) {
    console.error(`Radio path failed before restoration: ${error.stack}`);
    throw error;
  } finally {
    restoring = true;
    try { if (changed) await restore(original); }
    finally {
      try { await relay?.close(); }
      finally {
        try { await listener?.stop(); }
        finally {
          try { await server?.stop(); }
          finally {
            rmSync(dir, { recursive: true, force: true });
            process.off('SIGINT', requestStop); process.off('SIGTERM', requestStop);
          }
        }
      }
    }
  }
}
main().catch((error) => { console.error(error.stack); process.exitCode = 1; });
