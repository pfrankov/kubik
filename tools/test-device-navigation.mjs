#!/usr/bin/env node
// Real Back routes, preserving settings. GPIO debounce is covered by test-navigation.py.
import assert from 'node:assert/strict';
import { assertIdleCapture } from './test-device/diagnostics.mjs';
import {setTimeout as sleep} from 'node:timers/promises';
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
async function main() {
  const original=await info();
  assert.ok(original.guide_done, 'configured device required'); assertIdleCapture(original);
  try {
    if(original.menu)await sim('menu');
    if((await info()).screen_dark)await sim('pwr');
    await until(s=>!s.menu&&!s.screen_dark,'main screen');
    await sim('boot');await sleep(200);assert.equal((await info()).volume,original.volume);
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
    await sim('pair',{code:''});await control({cmd:'card',text:''});
    let state=await info();if(state.setup_ap)await sim('boot');
    state=await info();if(state.menu!==original.menu){await sim('menu');await sleep(550)}
    state=await info();if(state.screen_dark!==original.screen_dark){await sim('pwr');await sleep(250)}
    const restored=await info();
    for(const key of ['ssid','url','key','volume','brightness','character','guide_done','menu','screen_dark'])
      assert.equal(restored[key],original[key],`${key} changed`);
    assertIdleCapture(restored);
  }
}
main().catch(error=>{console.error(error.message);process.exitCode=1});
