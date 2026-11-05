#!/usr/bin/env node
// Native rapid-tap/rematch stack regression. Isolated Lab progress; no network or provider calls.
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { deviceGeometry } from './test-device/geometry.mjs';
import { assertIdleCapture, waitForDeviceState, setDeviceScreen } from './test-device/diagnostics.mjs';
import { restoreNavigationState } from './test-device-navigation.mjs';
import { TessContinuity, repeatScatter } from './test-device/tess-memory.mjs';

const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const response = await fetch(`${url}/config`, {method:'POST', headers:{'content-type':'application/json'},
    body:JSON.stringify(body), signal:AbortSignal.timeout(5000)});
  assert.equal(response.status, 200);
  const state = await response.json(); assert.equal(state.ok, true); return state;
}
const info = () => control({cmd:'info'});
const sim = (ev, fields = {}) => control({cmd:'sim', ev, ...fields});
const game = () => sim('game-state');
const diagnostics = () => control({cmd:'bridge-diagnostics'});
async function wait(read, predicate, label, timeoutMs = 5000) {
  return waitForDeviceState(read, predicate, label, {timeoutMs, intervalMs:80});
}
async function openAdult(g) {
  if (!(await info()).menu) await sim('menu');
  await wait(info, state => state.menu, 'Settings'); await sleep(600);
  for (let n = 0; n < 5; n++) { await sim('tap', {x:90,y:36}); await sleep(90); }
  await sim('hold', {x:90,y:36});
  await wait(info, state => state.screen_lab, 'Screen Lab');
  // Growth: tesseract is item25, page8/row1, with no saved progress writes.
  for (let page = 0; page < 8; page++) { await sim('tap', {x:356,y:g.footer}); await sleep(90); }
  await sim('tap', {x:240,y:144 + Math.trunc((g.height - 216) / 3)});
  await wait(info, state => state.lab_screen === 'Growth: tesseract', 'adult Tess preview');
  await sleep(3500);
  await wait(game, state => state.preview && state.available && state.form === 3, 'formed adult preview');
}
async function closeLab() {
  if (!(await info()).screen_lab) return;
  await sim('boot'); await wait(info, state => state.lab_screen === 'Screen Lab', 'Lab catalog');
  await sim('boot'); await wait(info, state => !state.screen_lab, 'Lab exit');
}
async function main() {
  const original = await info(), g = deviceGeometry(original);
  assert.equal(original.character, 'Tess', 'Tess firmware required');
  assert.equal(original.screen_lab, false, 'start outside Screen Lab');
  assert.equal(original.live_active, false); assertIdleCapture(original);
  for (const key of ['card_open','pair_visible','agent_open','sound_menu'])
    assert.equal(original[key], false, `close ${key} before the isolated memory probe`);
  const saved = (await game()).saved;
  const proof = new TessContinuity(original, await diagnostics(), performance.now());
  const check = async () => proof.check(await info(), await diagnostics(), performance.now());
  const tap = async () => {
    await sim('pointer', {down:true,x:240,y:g.tessY}); await sleep(65);
    await sim('pointer', {down:false,x:240,y:g.tessY}); await sleep(100);
  };
  try {
    await setDeviceScreen(info, sim, false);
    if ((await info()).setup_ap) {
      await sim('boot'); await wait(info, state => !state.setup_ap, 'initial setup closed before memory probe');
    }
    await openAdult(g);
    const prepare = async cycle => {
      if (!cycle) return;
      await sim('boot'); await wait(info, state => state.lab_screen === 'Screen Lab', 'Lab catalog');
      await sim('tap', {x:240,y:144 + Math.trunc((g.height - 216) / 3)});
      await wait(info, state => state.lab_screen === 'Growth: tesseract', 'fresh adult preview');
      await sleep(3500);
    };
    await repeatScatter({tap, game, check, wait, pause:sleep, prepare});
  } finally {
    await sim('pointer', {down:false,x:240,y:g.tessY});
    await closeLab();
    await restoreNavigationState(original, {read:info, event:sim, command:control,
      wait:(predicate,label) => wait(info,predicate,label), pause:sleep});
    const after = await info();
    for (const key of ['device','key','ssid','url','voice_mode','volume','ui_volume','brightness','guide_done'])
      assert.equal(after[key], original[key], `${key} changed`);
    assert.equal((await game()).saved, saved, 'player discoveries changed');
    await check();
  }
  console.log('PASS Tess stack: 24 native rapid taps, four scatter/form reversals, continuous uptime, zero bridge faults/reboots; baseline and saved progress restored');
}
main().catch(error => {console.error(error.stack); process.exitCode = 1;});
