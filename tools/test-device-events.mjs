#!/usr/bin/env node
// Local UI checks only: no provider calls; restore preferences, menu and screen power.
import assert from 'node:assert/strict';
import {setTimeout as sleep} from 'node:timers/promises';
import {assertIdleCapture} from './test-device/diagnostics.mjs';
const base=process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const response=await fetch(`${base}/config`,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body),signal:AbortSignal.timeout(5000)});
  assert.equal(response.status,200);const state=await response.json();assert.equal(state.ok,true);return state;
}
const info=()=>control({cmd:'info'});
const sim=(ev,extra={})=>control({cmd:'sim',ev,...extra});
const tap=(x,y)=>sim('tap',{x,y});
async function until(check,label) {
  for(let n=0;n<50;n++){const state=await info();if(check(state))return state;await sleep(100);}
  throw Error(`Timed out: ${label}`);
}
async function open() {
  const fresh=!(await info()).menu;
  if(fresh)await sim('menu');
  await until(s=>s.menu_ready,'Settings animation');
  if(fresh){await tap(272,36);assert.equal((await info()).events_open,false,'public Settings hides Events');}
  for(let n=0;n<5;n++)await tap(420,36);
  await tap(272,36);await until(s=>s.events_open,'Events');
}
async function populate(count,prefix) {
  for(let n=0;n<count;n++){await control({cmd:'card',text:`${prefix} ${n}`});await sleep(120);}
}
async function main() {
  const before=await info();assertIdleCapture(before);assert.ok(!before.live_active&&!before.screen_lab&&!before.card_open);
  assert.ok(before.online && before.events_count>=1,'authenticated welcome should be in history');
  const preserved=['volume','ui_volume','brightness','ssid','url','key','guide_done'];
  try {
    if(before.screen_dark)await sim('pwr');
    if(before.menu){await sim('menu');await until(s=>!s.menu,'initial home');}
    // Populate readable examples through the existing local text-card debug command.
    await populate(6,'Local journal check');
    await sim('boot');await open();
    assert.ok((await info()).events_count>=6);
    await sim('drag',{x:240,y:370,first:true});await sim('drag',{x:240,y:140});
    await until(s=>s.events_offset>=2,'swipe history');
    await tap(240,173);assert.equal((await info()).events_detail,false,'release after drag cannot open details');
    await sim('drag',{x:240,y:173,first:true});await tap(240,173);await until(s=>s.events_detail,'message details');
    await populate(16,'History rollover check');
    await until(s=>s.events_expired,'read message expires instead of changing');
    await control({cmd:'card',text:''});await sleep(150);
    await sim('boot');await until(s=>s.events_open&&!s.events_detail,'BOOT to list');
    await tap(240,92);await until(s=>s.event_overlay!==before.event_overlay,'overlay toggle');
    await sim('boot');await until(s=>!s.events_open&&s.menu,'BOOT to Settings');
    await sim('menu');await until(s=>!s.menu,'overlay home');
    const during=await info();assertIdleCapture(during);assert.equal(during.live_active,false);
    await open();await tap(240,92);await until(s=>s.event_overlay===before.event_overlay,'restore overlay');
    await sim('boot');await sim('menu');await until(s=>!s.menu,'close Settings');
    const after=await info();for(const key of preserved)assert.equal(after[key],before[key],`${key} changed`);
    assert.equal(after.heap_failures,before.heap_failures);assert.ok(after.online);
    console.log('device Events: authenticated welcome, text entries, swipe, details, expiry, BOOT, overlay and preserved settings passed');
  } finally {
    let state=await info();
    if(state.events_open){if(state.events_detail)await sim('boot');await sim('boot');}
    state=await info();
    if(state.event_overlay!==before.event_overlay){await open();await tap(240,92);await sleep(200);await sim('boot');}
    if((await info()).menu!==before.menu){await sim('menu');await until(s=>s.menu===before.menu,'restore menu');}
    if((await info()).screen_dark!==before.screen_dark){await sim('pwr');await until(s=>s.screen_dark===before.screen_dark,'restore screen');}
  }
}
main().catch(error=>{console.error(error.message);process.exitCode=1;});
