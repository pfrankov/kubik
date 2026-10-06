#!/usr/bin/env node
// Physical RX/ADC proof through USB control and local parrot; never invokes a paid provider.
import assert from 'node:assert/strict';
import { assertIdleCapture } from './test-device/diagnostics.mjs';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

const controlUrl = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const response = await fetch(`${controlUrl}/config`, {
    method: 'POST', headers: { 'content-type': 'application/json', connection: 'close' },
    body: JSON.stringify(body), signal: AbortSignal.timeout(5000),
  });
  assert.equal(response.status, 200);
  const state = await response.json();
  assert.equal(state.ok, true);
  return state;
}
const sim = (ev, extra = {}) => control({ cmd: 'sim', ev, ...extra });
const info = () => control({ cmd: 'info' });
async function until(check, label) {
  for (let attempt = 0; attempt < 100; attempt++) {
    const state = await info();
    if (check(state)) return state;
    await sleep(200);
  }
  throw new Error(`Timed out: ${label}`);
}
async function idleProof(label) {
  const before = await info();
  assertIdleCapture(before);
  await sleep(1200);
  const after = await info();
  assertIdleCapture(after);
  if (!before.wake_listening && !after.wake_listening) assert.equal(after.mic_reads, before.mic_reads, `${label}: reads while disabled`);
  assert.equal(after.mic_idle_reads, 0, `${label}: unexpected idle reads`);
  console.log(`PASS ${label}: no uploading capture; passive Tess or ADC/RX off; idle reads 0`);
}
async function speakLocal() {
  await new Promise((resolve, reject) => {
    const child = spawn('say', ['-v', 'Milena', 'Кубик, проверяем запись по кнопке. Микрофон работает только сейчас.'], { stdio: 'ignore' });
    child.once('error', reject);
    child.once('close', (code) => code ? reject(new Error('say failed')) : resolve());
  });
}
async function main() {
  const original = await info();
  const server = spawn(process.execPath, ['tools/usb-bridge/parrot-server.mjs', '18999'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let ready = false;
  let connected = false;
  let played = false;
  server.stdout.on('data', (data) => {
    if (String(data).includes('parrot on')) ready = true;
    if (String(data).includes('device connected')) connected = true;
    if (String(data).includes('"t":"played"')) played = true;
  });
  try {
    for (let attempt = 0; attempt < 30 && !ready; attempt++) await sleep(100);
    assert.equal(ready, true, 'local parrot not ready');
    await sim('wifi', { ms: 120000 });
    await until((state) => connected && state.online && state.via === 'usb', 'authenticated local parrot USB session');
    if ((await info()).screen_dark) await sim('pwr');
    if ((await info()).menu) await sim('menu');
    await idleProof('awake idle');
    await sim('menu');
    await idleProof('settings menu');
    await sim('menu');
    const before = await info();
    await sim('ptt_down');
    await until((state) => state.mic_enabled && state.mic_rx_enabled, 'PTT ADC/RX capture opens');
    await speakLocal();
    const recorded = await info();
    assert.ok(recorded.mic_reads > before.mic_reads, 'PTT did not read samples');
    await sim('ptt_up');
    await until((state) => !state.mic_enabled, 'PTT capture closes');
    console.log('PASS PTT opens ADC/RX and reads physical microphone samples');
    await idleProof('recording ended / echoed speech');
    await until(() => played, 'physical echo playback acknowledgement');
    console.log('PASS speaker playback acknowledged while microphone remains off');
    await sim('pwr');
    await until((state) => state.screen_dark, 'screen off');
    await idleProof('screen off');
    await until((state) => state.audio_dozing, 'codec/TX doze after completed speech');
    console.log('PASS completed speech releases codec/TX for dark-screen doze');
    await sim('pwr');
    await idleProof('screen wakes');
    await until((state) => !state.audio_dozing, 'codec/TX awake after screen wake');
    console.log('PASS codec/TX wake again without microphone capture');
  } finally {
    server.kill('SIGTERM');
    await sim('wifi', { ms: 1 });
    const restored = await until((state) => original.online ? state.online && state.via === original.via : !state.online, 'original connection');
    for (const key of ['volume', 'brightness', 'ssid', 'url', 'key', 'character']) assert.equal(restored[key], original[key], `${key} changed`);
    if (restored.screen_dark !== original.screen_dark) await sim('pwr');
  }
}
main().catch((error) => { console.error(error.message); process.exitCode = 1; });
