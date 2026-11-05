#!/usr/bin/env node
// Real firmware + local protocol server; no paid providers or persistent device changes.
// Bridge: ws://127.0.0.1:18999/kubik/v1, or kubik://127.0.0.1:18999 with KUBIK_TEST_TLS_IDENTITY.
import assert from 'node:assert/strict';
import { createUsbListener } from './test-device/usb-listener.mjs';
import { fixtureBaseline, restoreFixture, routeRestored } from './test-device/fixture-baseline.mjs';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';
import { KubikServer } from '../openclaw-kubik/src/server.js';
import { decodeDeviceKey } from '../openclaw-kubik/src/protocol.js';
import { account, fakeEngine } from '../openclaw-kubik/test/helpers.js';

async function control(command) {
  const response = await fetch('http://127.0.0.1:18791/config', {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify(command), signal: AbortSignal.timeout(5000),
  });
  assert.equal(response.status, 200);
  const result = await response.json();
  assert.equal(result.ok, true);
  return result;
}

async function until(check, label, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await check()) return;
    await sleep(250);
  }
  throw new Error(`Timed out: ${label}`);
}

async function darken() {
  await control({ cmd: 'card', text: '' });
  await sleep(300);
  if (!(await control({ cmd: 'info' })).screen_dark) await control({ cmd: 'sim', ev: 'pwr' });
  await until(async () => (await control({ cmd: 'info' })).screen_dark, 'screen off');
}

async function checkCronPassive(server) {
  let running = 0;
  server.cron = { summary: () => ({ running, next: -1 }) };
  server.refreshCron();
  await sleep(100);
  await darken();
  server.refreshCron();
  await sleep(300);
  assert.equal((await control({ cmd: 'info' })).screen_dark, true);
  const idleBefore = (await control({ cmd: 'info' })).idle_ms;
  running = 1;
  server.refreshCron();
  await sleep(300);
  assert.equal((await control({ cmd: 'info' })).screen_dark, true, 'background cron must not wake');
  server.refreshCron();
  await sleep(300);
  assert.equal((await control({ cmd: 'info' })).screen_dark, true);
  running = 2;
  server.refreshCron();
  await sleep(300);
  assert.equal((await control({ cmd: 'info' })).screen_dark, true, 'new background cron must not wake');
  running = 0;
  server.refreshCron();
  assert.ok((await control({ cmd: 'info' })).idle_ms >= idleBefore, 'cron reset idle timer');
  console.log('PASS cron snapshots update status without waking the screen');
}

async function main() {
  const { original, offlineFixture } = await fixtureBaseline(() => control({ cmd: 'info' }));
  assert.ok(offlineFixture || original.online && original.via === 'wifi', 'Start with the normal Wi-Fi agent or an explicit offline TLS USB fixture');
  const listener = createUsbListener({ allowPlain: original.url.startsWith('wss://') && !original.server_pinned });
  const { http, bind } = listener;
  const stateDir = mkdtempSync(join(tmpdir(), 'kubik-notify-device-'));
  const identity = `${original.device}:${decodeDeviceKey(original.key).fingerprint}`;
  const engine = fakeEngine();
  engine.canSpeak = false;
  const pairing = { allowed: async () => [identity], upsert: async () => { throw new Error('Unknown device on test server'); } };
  const server = new KubikServer({ account: account(), pairing,
    notificationPath: join(stateDir, 'notifications.json'), engineFactory: () => engine,
    dispatch: async () => {}, log: message => console.log(`HOST ${message}`) });
  http.on('upgrade', (req, socket, head) => server.handleUpgrade(req, socket, head,
    { via: 'usb-test', bind, remote: '127.0.0.1' }));
  try {
    server.start();
    await listener.listen();
    const queued = await server.notify(original.device, 'Test wake from queued event');
    assert.equal(queued.status, 'queued');
    assert.equal(queued.durable, true);
    await darken();
    await control({ cmd: 'sim', ev: 'wifi', ms: 90000 });
    // The bridge backs off for at most 30 s between unavailable fixture listeners.
    await until(() => server.onlineDevices.includes(original.device), 'USB authentication', 45000);
    await until(async () => !(await control({ cmd: 'info' })).screen_dark, 'queued notification wakes screen');
    console.log('PASS queued notification reaches actual firmware and wakes the dark screen');
    await darken();
    engine.canSpeak = true;
    const played = await server.notify(original.device, 'Test playback acknowledgement');
    assert.equal(played.status, 'played');
    assert.equal((await control({ cmd: 'info' })).screen_dark, false);
    console.log('PASS spoken notification wakes screen and returns the actual matching played ACK');
    await checkCronPassive(server);
  } finally {
    await restoreFixture({ offlineFixture,
      close: async () => {
        try { await server.stop(); }
        finally {
          try { await listener.close(); }
          finally { rmSync(stateDir, { recursive: true, force: true }); }
        }
      },
      restore: async () => {
        await control({ cmd: 'card', text: '' });
        await control({ cmd: 'sim', ev: 'wifi', ms: 1 });
        await until(async () => routeRestored(await control({ cmd: 'info' }), original), 'original connection restored');
        if (original.screen_dark !== (await control({ cmd: 'info' })).screen_dark)
          await control({ cmd: 'sim', ev: 'pwr' });
        const restored = await control({ cmd: 'info' });
        for (const key of ['device', 'key', 'url', 'agent', 'ssid', 'volume', 'ui_volume', 'brightness',
          'character', 'server_pinned', 'guide_done', 'screen_dark'])
          assert.equal(restored[key], original[key], `${key} changed`);
        console.log('PASS original route, saved settings, paired identity and screen restored');
      } });
  }
}

main().catch((error) => { console.error(error.message); process.exitCode = 1; });
