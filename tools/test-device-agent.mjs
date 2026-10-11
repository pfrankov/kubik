#!/usr/bin/env node
// Physical Agent UI regression through USB control. No recording or inference calls.
// Opt-in selections change only device settings, never invoke a model, and restore the original.
import assert from 'node:assert/strict';
import { assertIdleCapture } from './test-device/diagnostics.mjs';
import { setTimeout as sleep } from 'node:timers/promises';
const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
const select = process.argv.includes('--select');
const voiceSelect = process.argv.includes('--voice-select');
const modeSelect = process.argv.includes('--mode-select');
async function control(body) {
  const r = await fetch(`${url}/config`, {method:'POST', headers:{'content-type':'application/json'},
    body:JSON.stringify(body), signal:AbortSignal.timeout(5000)});
  assert.equal(r.status, 200);
  const s = await r.json(); assert.equal(s.ok, true); return s;
}
const info = async () => {
  const status=await control({cmd:'info'}), first=await control({cmd:'models',offset:0});
  const second=first.models_next===16 ? await control({cmd:'models',offset:16}) : null;
  return {...status,...first,agent_models:[...first.agent_models,...(second?.agent_models??[])]};
};
const sim = (ev, extra={}) => control({cmd:'sim', ev, ...extra});
async function until(check, label) {
  for (let i=0; i<60; i++) {
    const s=await info(); if(check(s)) return s; await sleep(200);
  }
  throw Error(`Timed out: ${label}`);
}
async function tap(x,y) { await sim('tap',{x,y}); await sleep(150); }
function preserved(s, original) {
  for(const k of ['volume','brightness','character','ssid','url','key']) assert.ok(s[k]===original[k],`${k} changed`);
  assertIdleCapture(s);
}
async function openAgent() {
  let s=await info(); if(s.screen_dark){await sim('pwr'); await sleep(200)}
  s=await info(); if(s.menu){await sim('menu');await sleep(200)}
  await sim('menu');await sleep(550); await tap(356,144);
  return until(s=>s.agent_open&&s.agent_view==='overview'&&s.agent_status==='ready'&&s.agent_model, 'Agent overview');
}
async function dragRejected(original,x,y) {
  const before=await info();
  await sim('drag',{x,y,first:true}); await sim('drag',{x,y:y+30});
  await tap(x,y); const after=await info();
  assert.equal(after.agent_view,before.agent_view); assert.equal(after.agent_model,before.agent_model);
  preserved(after,original);
}
// The list scrolls by finger: rows sit 78 px apart from y 117, and coast after the finger leaves.
const ROW_Y=i=>117+i*78;
async function settled() {
  let last=(await info()).agent_scroll;
  for(let i=0;i<40;i++) { await sleep(120); const now=(await info()).agent_scroll; if(now===last) return now; last=now; }
  throw Error('list kept scrolling');
}
async function flick(from,to) {
  await sim('drag',{x:240,y:from,first:true}); await sim('drag',{x:240,y:from+(to<from?-14:14)});
  await sleep(40); await sim('drag',{x:240,y:(from+to)/2}); await sleep(40); await sim('drag',{x:240,y:to});
  await sim('drag',{x:240,y:to,last:true}); return settled();
}
async function fullList() {
  return until(s=>s.agent_view==='models'&&s.agent_status==='ready'&&s.agent_loaded===s.agent_listed&&s.agent_loaded>0,'whole model list loaded');
}
async function moveTo(index) {
  for(let i=0;i<40;i++) {
    const y=ROW_Y(index)-(await info()).agent_scroll;
    if(y>=130&&y<=420) return y;
    await flick(y>420?400:200, y>420?220:380);
  }
  throw Error(`row ${index} not reachable`);
}
async function browse(original) {
  await dragRejected(original,127,196); await dragRejected(original,353,196);
  const modes = await voiceModes();
  await tap(240, 144 + ['classic','realtime','live'].indexOf(modes.voice_mode) * 124);
  if (modes.voice_mode === 'classic') {
    await until(s=>s.agent_view==='classic'&&s.agent_status==='ready','STT settings');
    for (const [target,y] of [['stt',214],['tts',366]]) {
      await tap(240,y); await until(s=>s.agent_view==='models'&&s.agent_target===target&&s.agent_status==='ready',`${target} models`);
      await sim('boot'); await until(s=>s.agent_view==='classic'&&s.agent_status==='ready','BOOT to STT settings');
    }
  } else await until(s=>s.agent_view==='models'&&s.agent_target==='voice'&&s.agent_status==='ready','shared voice models');
  await backToAgent();
  await tap(240,196);let s=await until(s=>s.agent_view==='models'&&s.agent_status==='ready','model picker');
  s=await fullList(); const ids=[...s.agent_models];
  assert.equal(ids.length,s.agent_listed); preserved(s,original);
  if(ids.length>5) {
    const top=await settled(); assert.equal(top,0);
    const first=await flick(400,150); assert.ok(first>0,'drag scrolls the list');
    for(let i=0;i<8;i++) await flick(150,420);
    assert.equal(await settled(),0,'list stops at the first row');
    // A short drag released at speed keeps gliding: far past where the finger left it.
    await sim('drag',{x:240,y:400,first:true}); await sim('drag',{x:240,y:384}); await sleep(40);
    await sim('drag',{x:240,y:350}); await sleep(40); await sim('drag',{x:240,y:316});
    await sim('drag',{x:240,y:316,last:true}); const atRelease=(await info()).agent_scroll;
    const glided=await settled(); assert.ok(glided>atRelease+30,'list keeps coasting after release');
    for(let i=0;i<8;i++) await flick(150,420);
    assert.equal(await settled(),0,'list stops at the first row');
  }
  await dragRejected(original,127,164);
  console.log(`PASS overview, mode-first voice catalogs, ${ids.length} models by inertial scroll; drags preserve sliders/model`);
  return ids;
}
async function choose(id,ids,target='agent') {
  const index=ids.indexOf(id);assert.ok(index>=0,`${id} absent from catalog`);
  const y=await moveTo(index);
  await sim('drag',{x:240,y,first:true});await sim('drag',{x:244,y:y+4});
  await tap(240,y);
  return until(s=>(target==='agent'?s.agent_model:s.voice_model)===id&&s.agent_target===target&&s.agent_status==='ready',`persisted ${id}`);
}
async function roundTrip(originalModel,ids) {
  const target=originalModel==='openai/gpt-6-luna'?'openai/gpt-6.1-sol':'openai/gpt-6-luna';
  try {await choose(target,ids);console.log('PASS changed model with large touch target')}
  finally {await choose(originalModel,ids);console.log('PASS original paired-session model restored')}
}
async function voiceModes() {
  await tap(240,350);
  return until(s=>s.agent_view==='voice_modes'&&s.agent_target==='mode'&&s.agent_status==='ready','voice modes');
}
async function backToAgent() {
  for (let i=0;i<4;i++) {
    const s=await info(); if (s.agent_view==='overview') return until(s=>s.agent_status==='ready','Agent readback');
    await sim('boot'); await sleep(250);
  }
  throw Error('BOOT did not return to Agent');
}
async function voicePicker() {
  const modes=await voiceModes();
  await tap(240,144+['classic','realtime','live'].indexOf(modes.voice_mode)*124);
  return until(s=>s.agent_view==='models'&&s.agent_target==='voice'&&s.agent_status==='ready','native voice models');
}
async function voiceRoundTrip(agentModel) {
  const input=await voicePicker(), original=input.voice_model;
  const alternative=input.agent_models.find(id=>id!==original);
  assert.ok(alternative, 'Need two server voice models for the selection contrast');
  try {
    const changed=await choose(alternative,(await fullList()).agent_models,'voice');
    assert.equal(changed.agent_model,agentModel); await backToAgent();
    const reopened=await voicePicker(); assert.equal(reopened.voice_model,alternative);
    await choose(original,(await fullList()).agent_models,'voice');
    assert.equal((await info()).agent_model,agentModel);
    console.log('PASS shared voice model: save, reopen, agent isolation and restore');
  } finally {
    await openAgent(); const current=await voicePicker();
    if(current.voice_model!==original)await choose(original,(await fullList()).agent_models,'voice');
    await backToAgent();
  }
}
async function chooseMode(mode) {
  await openAgent(); const modes=await voiceModes();
  const index=modes.agent_models.indexOf(mode); assert.ok(index>=0,`Missing mode ${mode}`);
  await tap(240,144+index*124);
  const s=await until(s=>s.voice_mode===mode&&s.agent_status==='ready'&&
    s.agent_view===(mode==='classic'?'classic':'models'),'mode saved and relevant settings opened');
  assertIdleCapture(s); return s;
}
async function modeRoundTrip(original) {
  try {
    await openAgent(); const before=await voiceModes();
    const target=original.voice_mode==='live'?'realtime':'live';
    await sim('tap',{x:240,y:144+before.agent_models.indexOf(target)*124});
    await sim('boot');
    await until(s=>s.agent_view==='overview'&&s.agent_status==='ready'&&s.voice_mode===target,'BOOT during mode save');
    console.log('PASS immediate BOOT during save; parent catalog recovers without Busy');
    for (const mode of ['classic','live','realtime']) {
      const s=await chooseMode(mode); assert.equal(s.agent_model,original.agent_model);
      await backToAgent(); const reopened=await voiceModes(); assert.equal(reopened.voice_model,mode);
    }
    console.log('PASS STT / GPT Live / Realtime changes, readback and agent isolation; no inference');
  } finally { await chooseMode(original.voice_mode); await backToAgent(); }
}
async function main() {
  const original=await until(s=>s.online&&s.key,'initialized paired device');
  try {
    const loaded=await openAgent();
    if(modeSelect) await modeRoundTrip(loaded);
    if(voiceSelect) {
      assert.equal(loaded.voice_mode,'realtime','--voice-select requires Realtime with two configured voice models');
      await voiceRoundTrip(loaded.agent_model);
    }
    const ids=await browse(original);
    if(select)await roundTrip(loaded.agent_model,ids);
    await sim('boot');await until(s=>s.agent_view==='overview','back to overview');
    await sim('boot');const end=await until(s=>!s.agent_open&&s.menu,'back to device settings');
    preserved(end,original);
    // Contrasting case: the underlying settings slider still responds to a drag.
    const y=original.volume>=50?300:180;
    await sim('drag',{x:72,y:226,first:true});
    await sim('drag',{x:72,y});await tap(72,y);
    assert.notEqual((await info()).volume,original.volume);
    await control({cmd:'set',volume:original.volume});await sim('menu');
    preserved(await info(),original);
    console.log('PASS settings slider still works; original volume/brightness, network, keys and microphone state preserved');
  } finally {
    const s=await info();
    if(s.volume!==original.volume)await control({cmd:'set',volume:original.volume});
    if(s.menu!==original.menu)await sim('menu');
    if(s.screen_dark!==original.screen_dark)await sim('pwr');
  }
}
main().catch(e=>{console.error(e.message);process.exitCode=1});
