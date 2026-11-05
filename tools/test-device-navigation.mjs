#!/usr/bin/env node
// Real Back routes, preserving settings. GPIO debounce is covered by test-navigation.py.
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { assertIdleCapture, flushStats, waitForDeviceLink, waitForIdleCapture } from './test-device/diagnostics.mjs';
import {setTimeout as sleep} from 'node:timers/promises';
import { prepareHome } from './test-device-guide.mjs';
const url=process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const r=await fetch(`${url}/config`,{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify(body),signal:AbortSignal.timeout(5000)});
  assert.equal(r.status,200);const result=await r.json();assert.equal(result.ok,true);return result;
}
const info=()=>control({cmd:'info'});
const sim=(ev,extra={})=>control({cmd:'sim',ev,...extra});
async function until(check,label) {
  for(let n=0;n<160;n++) {const state=await info();if(check(state))return state;await sleep(100)}
  throw Error(`Timed out: ${label}`);
}
async function blocked(label) {
  const before=await until(s=>!s.mic_enabled&&!s.mic_rx_enabled&&!s.wake_listening,label);
  await sleep(200); const after=await info(); assert.equal(after.mic_reads,before.mic_reads,label);
}
async function darkScreen() {
  await sim('pwr');await until(s=>s.screen_dark,'screen dark');
  for(const ev of ['boot','menu','ptt_down','ptt_up','tap','hold','pet','shake','pickup','drag','setup']) {
    await sim(ev,{x:240,y:240});await sleep(100);
    const state=await info();
    assert.ok(state.screen_dark&&!state.menu&&!state.mic_open&&!state.auto_recording&&!state.live_active,`${ev} acts in darkness`);
  }
  await control({cmd:'card',text:' \t\r\n'});await sleep(200);
  assert.ok((await info()).screen_dark,'blank card wakes screen');
  await sim('mode',{mode:0,ms:600});await sleep(250);
  assert.ok((await info()).screen_dark,'debug tick wakes screen');
  await sim('mode',{mode:0,ms:0});
  await sim('pwr');await until(s=>!s.screen_dark,'PWR wakes screen');
  await sim('pwr');await until(s=>s.screen_dark,'second dark screen');
  await control({cmd:'card',text:'A reply is ready to read.'});
  await until(s=>!s.screen_dark&&s.card_open,'readable reply wakes screen');
  await sim('boot');await until(s=>!s.card_open,'close reply');
}
async function settleBootReply({ read, event, pause, stats }) {
  await stats(); // Remove the recording frame window before observing the completed reply.
  const deadline = Date.now() + 20000;
  for (let n = 0; n < 40 && Date.now() < deadline; n++) {
    await pause(350);
    const state = await read();
    if (state.card_open) { await event('boot'); continue; } // Back dismisses a text reply; it cannot start voice here.
    const window = await stats();
    if (window.modes === (1 << 1) && !state.mic_open && !state.live_active) {
      await waitForIdleCapture(read);
      return;
    }
  }
  throw Error('BOOT voice turn did not return to a displayed idle home');
}
export async function homeBootProof(original, { read = info, event = sim, wait = until, pause = sleep,
  stats = () => flushStats({ sim: event }) } = {}) {
  assert.ok(['esp32c6', 'esp32s3'].includes(original.chip), 'Known firmware chip required');
  if (original.chip === 'esp32c6') {
    await event('boot'); await pause(200);
    assert.equal((await read()).volume, original.volume);
    return;
  }
  assert.equal(original.voice_mode, 'classic', 'S3 navigation proof requires Classic STT');
  assert.ok(original.stt_known && original.stt_available, 'S3 navigation proof requires local STT capability');
  await event('boot');
  const started = await wait(s => s.mic_open && s.mic_enabled && s.mic_rx_enabled, 'short BOOT starts capture');
  await pause(600); // Beyond LATCH_MS: the released button must remain latched.
  const latched = await read();
  assert.ok(latched.mic_open && !latched.auto_recording && !latched.live_active, 'BOOT did not latch Classic capture');
  assert.ok(latched.mic_reads > started.mic_reads, 'Latched BOOT did not read physical microphone samples');
  await event('boot');
  await wait(s => !s.mic_open && !s.auto_recording, 'second short BOOT closes capture');
  await settleBootReply({ read, event, pause, stats });
  assert.equal((await read()).volume, original.volume);
  console.log('PASS S3 BOOT: short press latches real capture; second press sends and returns to idle');
}
export async function restoreNavigationState(original, { read = info, event = sim, command = control,
  wait = until, pause = sleep } = {}) {
  if ((await read()).mic_open) {
    await event('tap', { x: 240, y: 240 });
    await wait(s => !s.mic_open, 'navigation cleanup closes recording');
  }
  await event('pair', { code: '' }); await command({ cmd: 'card', text: '' });
  if ((await read()).screen_dark) { await event('pwr'); await wait(s => !s.screen_dark, 'awake before restoring UI'); }
  if ((await read()).setup_ap) { await event('boot'); await wait(s => !s.setup_ap, 'close temporary setup'); }
  if ((await read()).menu) { await event('menu'); await wait(s => !s.menu, 'close temporary menu'); }
  if (original.menu) { await event('menu'); await wait(s => s.menu, 'restore original menu'); }
  if (original.setup_ap) {
    await event('setup'); await wait(s => Boolean(s.setup_ap), 'restore original setup AP');
  }
  if ((await read()).screen_dark !== original.screen_dark) { await event('pwr'); await pause(250); }
  const restored = await waitForIdleCapture(read);
  for (const key of ['ssid','url','key','volume','brightness','character','guide_done','menu','screen_dark'])
    assert.equal(restored[key],original[key],`${key} changed`);
  assert.equal(restored.setup_ap, original.setup_ap, 'initial setup AP changed');
}
async function main() {
  const original=await waitForDeviceLink(info);
  assert.ok(original.guide_done, 'configured device required'); assertIdleCapture(original);
  try {
    await prepareHome({read:info,event:sim,wait:until});
    await homeBootProof(original);
    await control({cmd:'card',text:'Back closes this card'});await until(s=>s.card_open,'card visible');
    await sim('boot');await until(s=>!s.card_open,'Back dismisses card');
    await sim('pair',{code:'TEST2345'});await until(s=>s.pair_visible,'pairing card visible'); await blocked('pairing disables listener');
    await sim('boot');await until(s=>!s.pair_visible,'Back hides pairing card');
    await sim('menu');await until(s=>s.menu,'hidden pairing permits Settings');
    await sim('boot');await until(s=>!s.menu,'Back closes Settings');
    await sim('pair',{code:''});
    await sim('setup');await until(s=>s.setup_ap,'setup opens'); await blocked('setup disables listener');
    await sim('boot');await until(s=>!s.setup_ap,'Back closes setup');
    await until(s=>s.online,'saved connection restored');
    await darkScreen();
    const state=await info();assert.equal(state.menu,false);assert.equal(state.volume,original.volume);
    console.log('PASS BOOT: main volume unchanged, text dismissed, pairing hidden, Settings accessible, setup closed; dark input ignored, only PWR/reply wakes');
  } finally {
    await restoreNavigationState(original);
  }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  main().catch(error=>{console.error(error.message);process.exitCode=1});
