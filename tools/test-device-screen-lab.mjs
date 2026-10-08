#!/usr/bin/env node
// Real hidden entry and isolated native previews through the device's USB control events.
import assert from 'node:assert/strict';
import {setTimeout as sleep} from 'node:timers/promises';
import {assertIdleCapture, waitForDeviceLink} from './test-device/diagnostics.mjs';
const url=process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const response=await fetch(`${url}/config`,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body),signal:AbortSignal.timeout(5000)});
  assert.equal(response.status,200);const state=await response.json();assert.equal(state.ok,true);return state;
}
const info=()=>control({cmd:'info'});
const sim=(ev,extra={})=>control({cmd:'sim',ev,...extra});
const tap=(x,y)=>sim('tap',{x,y});
async function until(check,label) {
  for(let n=0;n<80;n++) {const state=await info();if(check(state))return state;await sleep(100);}
  const state=await info();
  throw Error(`Timed out: ${label}; preview=${state.lab_screen}, menu=${state.menu}, dark=${state.screen_dark}, lab=${state.screen_lab}`);
}
async function catalog() {
  // KEY has already left any nested menu preview. Sim acknowledges enqueue,
  // so send BOOT once and observe its result before issuing another action.
  await sim('boot');
  await until(state=>state.lab_screen==='Screen Lab','BOOT to lab catalog');
}
async function enter() {
  for(let n=0;n<5;n++) {await tap(90,36);await sleep(90);}
  await sim('hold',{x:90,y:36});
  await until(state=>state.screen_lab,'hidden lab entry');
}
async function previewKey(index,names) {
  await sim('ptt_down');await sim('ptt_up');
  const next=index>=1&&index<=3?names[index+1]:index===14?'Home':
    [11,12,13,20].includes(index)?names[index]:index>=22?names[index]:'Recording';
  await until(state=>state.lab_screen===next,`KEY in ${names[index]}`);
  const state=await info();assert.ok(!state.mic_open&&!state.auto_recording&&!state.live_active,'preview KEY cannot record');
}
async function main() {
  const original=await waitForDeviceLink(info);assertIdleCapture(original);assert.ok(!original.screen_lab);
  const preserved=['volume','ui_volume','brightness','ssid','url','key','guide_done'];
  try {
    if(original.menu)await sim('menu');
    if((await info()).screen_dark)await sim('pwr');
    await sim('menu');await until(state=>state.menu,'Settings');
    await sleep(600);
    await sim('hold',{x:90,y:36});assert.ok(!(await info()).screen_lab,'hold alone must not unlock');
    await enter();
    const names=['Home','Connecting','Wi-Fi QR','Setup QR','Pairing','Settings','Sound','Agent','Voice modes','Models','Guide','Recording','Thinking','Speaking','GPT Live','Text reply','Offline points','Error','Background work','Reminder','Agent events','Event log','Growth: point','Growth: square','Growth: cube','Growth: tesseract','Echo: tier 1','Echo: tier 2','Echo: tier 3','Catch: tier 1','Catch: tier 2','Catch: tier 3'];
    for(let index=0;index<names.length;index++) {
      if(index && index%3===0)await tap(356,428);
      await tap(240,144+(index%3)*88);
      await until(state=>state.lab_screen===names[index],names[index]);
      await previewKey(index,names);
      await catalog();
    }
    await sim('boot');await until(state=>!state.screen_lab&&state.menu,'exit to Settings');
    await enter();await sim('pwr');await until(state=>!state.screen_lab&&state.screen_dark,'PWR closes lab');
    await sim('pwr');await sim('menu');await sleep(400);
    await sim('hold',{x:90,y:36});assert.ok(!(await info()).screen_lab,'closing Settings clears entry latch');
    await sim('menu');
    const after=await info();
    for(const field of preserved)assert.equal(after[field],original[field],`${field} changed`);
    assert.equal(after.heap_failures,original.heap_failures);
    assert.ok(after.online&&after.via===original.via,'connection preserved');
    console.log('device Screen Lab: hidden entry, 32 native previews, KEY replay/capture gate, BOOT/PWR exits and unchanged settings passed');
  } finally {
    const state=await info();
    if(state.screen_lab)await sim('menu');
    if((await info()).menu!==original.menu)await sim('menu');
    if((await info()).screen_dark!==original.screen_dark)await sim('pwr');
  }
}
main().catch(error=>{console.error(error.message);process.exitCode=1;});
