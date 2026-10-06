#!/usr/bin/env node
// Check the DFS configuration and active display clock; a USB query wakes CPU.
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { assertIdleCapture, setDeviceScreen } from './test-device/diagnostics.mjs';

const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const response = await fetch(`${url}/config`, { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body),
    signal: AbortSignal.timeout(5000) });
  assert.equal(response.status, 200);
  const result = await response.json(); assert.equal(result.ok, true); return result;
}
const info = () => control({ cmd: 'info' });
const sim = ev => control({ cmd: 'sim', ev });

async function sample(dark) {
  await setDeviceScreen(info, sim, dark);
  await sleep(1000); // allow display and codec locks to settle
  const frequencies = [];
  for (let n = 0; n < 20; n++) {
    const state = await info();
    assert.equal(state.screen_dark, dark);
    assert.equal(state.cpu_min_mhz, 40, 'DFS floor differs from the C6 crystal');
    frequencies.push(state.cpu_mhz);
    await sleep(200);
  }
  assert.ok(frequencies.every(value => [40, 80, 160].includes(value)), 'Unsupported frequency');
  console.log(`Measured CPU ${dark ? 'dark' : 'awake'}: ${JSON.stringify(frequencies)}`);
  if (!dark) assert.ok(frequencies.every(value => value === 160), 'Display lost its full-speed lock');
}

async function main() {
  const original = await info();
  assertIdleCapture(original);
  for (const key of ['menu', 'card_open', 'pair_visible', 'live_active']) assert.equal(original[key], false);
  assert.equal(original.online, true);
  try {
    await sample(false);
    await sample(true);
    await sample(false);
  } finally {
    await setDeviceScreen(info, sim, original.screen_dark);
    const restored = await info();
    for (const key of ['ssid', 'url', 'key', 'volume', 'brightness', 'character', 'screen_dark'])
      assert.equal(restored[key], original[key], `${key} changed`);
    assertIdleCapture(restored);
  }
  console.log('PASS CPU: DFS floor 40 MHz, awake/wake stay 160 MHz, configuration preserved; idle residency requires PM profiling');
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
