#!/usr/bin/env node
// Real elapsed idle time after a completed text reply. No provider calls.
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { assertIdleCapture } from './test-device/diagnostics.mjs';

const endpoint = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const r = await fetch(`${endpoint}/config`, { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body),
    signal: AbortSignal.timeout(5000) });
  assert.equal(r.status, 200);
  const result = await r.json(); assert.equal(result.ok, true); return result;
}
const info = () => control({ cmd: 'info' });
const sim = ev => control({ cmd: 'sim', ev });

async function checkIdle() {
  const started = Date.now();
  let dimmed = false, sleeping = false, cardClosed = false;
  let previousIdle = 0;
  while (Date.now() - started < 265000) {
    const state = await info();
    assert.equal(state.mic_open, false);
    if (!state.card_open) cardClosed = true;
    if (cardClosed) assert.ok(state.idle_ms >= previousIdle, `Unexpected activity reset: ${previousIdle} -> ${state.idle_ms} ms`);
    previousIdle = state.idle_ms;
    dimmed ||= state.screen_dimmed;
    sleeping ||= state.screen_sleeping;
    if (state.idle_ms > 122000) assert.equal(state.sfx_playing, false, 'Drowsiness or sleep made a sound');
    if (state.screen_dark) {
      assert.ok(dimmed && sleeping && cardClosed, 'Incomplete dim/sleep sequence');
      assert.equal(state.wake_listening, false);
      console.log(`PASS physical idle: card closed, dim/sleep/dark at ${state.idle_ms} ms, no sleepy/sleep SFX`);
      return;
    }
    await sleep(250);
  }
  throw Error('Did not complete automatic sleep');
}

async function restore(original) {
  await control({ cmd: 'card', text: '' }); await sleep(300);
  if ((await info()).screen_dark !== original.screen_dark) await sim('pwr');
  const restored = await info();
  for (const key of ['ssid', 'url', 'key', 'volume', 'brightness', 'guide_done', 'character', 'menu', 'screen_dark'])
    assert.equal(restored[key], original[key], `${key} changed`);
  assertIdleCapture(restored);
}

async function main() {
  const original = await info();
  assertIdleCapture(original);
  for (const [key, expected] of Object.entries({ online: true, guide_done: true, menu: false,
    card_open: false, pair_visible: false, live_active: false })) assert.equal(original[key], expected, `${key}: idle main screen required`);
  assert.equal(typeof original.idle_ms, 'number', 'Updated firmware required');
  try {
    if (original.screen_dark) await sim('boot');
    await control({ cmd: 'card', text: 'A completed text reply before automatic sleep.' });
    await sleep(500); assert.equal((await info()).card_open, true);
    await checkIdle();
    await sim('boot'); await sleep(500);
    assert.equal((await info()).screen_dark, false);
    console.log('PASS physical wake: BOOT restores main screen');
  } finally { await restore(original); }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
