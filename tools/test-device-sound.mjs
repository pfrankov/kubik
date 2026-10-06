#!/usr/bin/env node
// Local USB controls only: no provider calls, microphone activation or factory reset.
import assert from 'node:assert/strict';
import { setTimeout as pause } from 'node:timers/promises';
const control = process.env.KUBIK_TEST_CONTROL ?? 'http://127.0.0.1:18791';
async function command(payload) {
  const response = await fetch(`${control}/config`, { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(payload), signal: AbortSignal.timeout(5000) });
  const data = await response.json(); assert.equal(data.ok, true); return data;
}
const info = () => command({cmd:'info'});
const sim = (ev, data={}) => command({cmd:'sim', ev, ...data});
async function settled(test, message) {
  for (let n=0;n<30;n++) { try { const state=await info(); if(test(state)) return state; } catch {} await pause(200); }
  throw Error(message);
}
const original = await info();
assert.equal(original.live_active, false, 'active Live conversation');
assert.equal(original.auto_recording, false, 'active automatic recording');
assert.equal(original.mic_open, false, 'active microphone');
assert.equal(Number.isInteger(original.ui_volume), true, 'new firmware must expose ui_volume');
let closed = !original.menu;
try {
  if(original.menu) await sim('menu');
  await sim('mode', {mode:0, ms:0});
  await sim('menu'); await pause(650);
  assert.equal((await info()).menu, true);
  const before=await info();
  await sim('drag',{x:72,y:226,first:true});
  assert.equal((await info()).volume,before.volume,'touch-down changes volume');
  await sim('hold',{x:72,y:226}); await pause(100);
  assert.equal((await info()).sound_menu,true);
  await sim('tap',{x:348,y:before.ui_volume>=50?300:180});
  const ui=await settled(s=>s.ui_volume!==before.ui_volume,'interface slider did not change'); assert.equal(ui.volume,before.volume);
  await sim('tap',{x:132,y:before.volume>=50?300:180});
  const speech=await settled(s=>s.volume!==before.volume,'speech slider did not change'); assert.equal(speech.ui_volume,ui.ui_volume);
  await command({cmd:'set',volume:original.volume});
  assert.equal((await info()).ui_volume,ui.ui_volume,'host speech setting changes interface');
  await sim('boot'); await pause(100);
  const back=await info(); assert.equal(back.sound_menu,false); assert.equal(back.menu,true);
  await sim('boot'); await pause(300); assert.equal((await info()).menu,false);
  await command({cmd:'reboot'});
  const loaded=await settled(s=>s.ui_volume===ui.ui_volume && s.uptime_ms < original.uptime_ms,'interface volume did not survive reboot');
  assert.equal(loaded.sound_menu,false);
  console.log('device Sound: hold, independent sliders, set isolation, BOOT and persistence passed');
} finally {
  await settled(()=>true,'device did not return for restoration');
  const state=await info(); if(state.menu) await sim('menu');
  await command({cmd:'set',volume:original.volume,ui_volume:original.ui_volume,brightness:original.brightness});
  if(!closed) {await sim('menu');await pause(650);}
  const restored=await info(); assert.equal(restored.volume,original.volume); assert.equal(restored.ui_volume,original.ui_volume);
}
