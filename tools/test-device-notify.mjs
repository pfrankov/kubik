#!/usr/bin/env node
// Real firmware + local protocol server; no paid providers or persistent device changes.
// Requires the USB bridge targeting ws://127.0.0.1:18999/kubik/v1.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
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

async function until(check, label) {
  for (let n = 0; n < 80; n++) {
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
  const original = await control({ cmd: 'info' });
  const stateDir = mkdtempSync(join(tmpdir(), 'kubik-notify-device-'));
  const identity = `${original.device}:${decodeDeviceKey(original.key).fingerprint}`;
  const engine = fakeEngine();
  engine.canSpeak = false;
  const pairing = { allowed: async () => [identity], upsert: async () => { throw new Error('Unknown device on test server'); } };
  const server = new KubikServer({ account: account(), pairing,
    notificationPath: join(stateDir, 'notifications.json'), engineFactory: () => engine,
    dispatch: async () => {}, log: () => {} });
  const http = createServer((_req, res) => { res.statusCode = 404; res.end(); });
  http.on('upgrade', (req, socket, head) => server.handleUpgrade(req, socket, head,
    { via: 'usb-test', bind: 'none', remote: '127.0.0.1' }));
  try {
    server.start();
    await new Promise((resolve, reject) => { http.once('error', reject); http.listen(18999, '127.0.0.1', resolve); });
    const queued = await server.notify(original.device, 'Test wake from queued event');
    assert.equal(queued.status, 'queued');
    assert.equal(queued.durable, true);
    await darken();
    await control({ cmd: 'sim', ev: 'wifi', ms: 90000 });
    await until(() => server.onlineDevices.includes(original.device), 'USB authentication');
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
    try {
      await control({ cmd: 'card', text: '' }).catch(() => {});
      await control({ cmd: 'sim', ev: 'wifi', ms: 1 }).catch(() => {});
      if (original.screen_dark !== (await control({ cmd: 'info' })).screen_dark)
        await control({ cmd: 'sim', ev: 'pwr' });
    } finally {
      try { await server.stop(); }
      finally {
        try { await new Promise((resolve) => http.close(resolve)); }
        finally { rmSync(stateDir, { recursive: true, force: true }); }
      }
    }
    await until(async () => (await control({ cmd: 'info' })).via === 'wifi', 'original Wi-Fi route restored');
    const restored = await control({ cmd: 'info' });
    assert.equal(restored.ssid, original.ssid);
    assert.equal(restored.url, original.url);
    assert.equal(restored.key, original.key);
    console.log('PASS saved network, server address and paired identity preserved');
  }
}

main().catch((error) => { console.error(error.message); process.exitCode = 1; });
