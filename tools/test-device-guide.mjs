#!/usr/bin/env node
// Physical guide replay/completion. --first-run requires the automatic first welcome tour.
// --reboot proves saved completion survives reboot; --profile measures native Settings/guide frame windows.
// No voice/inference calls.
import assert from 'node:assert/strict';
import { assertIdleCapture } from './test-device/diagnostics.mjs';
import fs from 'node:fs';
import {setTimeout as sleep} from 'node:timers/promises';
const url=process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const r=await fetch(`${url}/config`,{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify(body),signal:AbortSignal.timeout(5000)});
  assert.equal(r.status,200);const s=await r.json();assert.equal(s.ok,true);return s;
}
const info=()=>control({cmd:'info'});
const sim=(ev,extra={})=>control({cmd:'sim',ev,...extra});
async function until(check,label,reconnecting=false) {
  const end=Date.now()+45000;
  while(Date.now()<end) {
    try {const s=await info();if(check(s))return s} catch(e) {if(!reconnecting)throw e}
    await sleep(250);
  }
  throw Error(`Timed out: ${label}`);
}
async function tap(x,y) {await sim('drag',{x,y,first:true});await sim('tap',{x,y});await sleep(200)}
function preserved(s,original) {
  for(const key of ['volume','brightness','character','ssid','url','key'])assert.equal(s[key],original[key],key);
  assertIdleCapture(s);
}
async function replay() {
  let s=await info();if(s.screen_dark){await sim('pwr');await sleep(200)}
  s=await info();if(s.menu){await sim('menu');await sleep(200)}
  await sim('menu');await sleep(550);await tap(356,226);
  return until(s=>s.agent_open&&s.agent_view==='guide'&&s.guide_step===0,'replayed guide');
}
const bridgeLog=process.env.KUBIK_BRIDGE_LOG ?? '/tmp/kubik-bridge.log';
const logSize=()=>fs.statSync(bridgeLog).size;
function logSince(offset) {
  const fd=fs.openSync(bridgeLog,'r'),buffer=Buffer.alloc(logSize()-offset);
  try {fs.readSync(fd,buffer,0,buffer.length,offset)} finally {fs.closeSync(fd)}
  return buffer.toString();
}
async function flushStats() {
  const offset=logSize();await sim('stats');
  for(let n=0;n<100;n++) {
    if(/main: present:/.test(logSince(offset)))return;
    await sleep(25);
  }
  throw Error('No display stats in bridge log');
}
async function frameWindow(label,action) {
  await flushStats();const offset=logSize();if(action)await action();
  await sleep(2300);await flushStats();
  const m=/display: ([\d.]+) fps .*max (\d+), (\d+) over 33.3 ms/.exec(logSince(offset));
  assert.ok(m,`${label}: missing display stats`);
  assert.ok(Number(m[1])>=29.5,`${label}: ${m[1]} fps`);
  assert.equal(Number(m[3]),0,`${label}: ${m[3]} late frames, maximum ${m[2]} us (limit 33333 us)`);
  console.log(`PASS ${label}: ${m[1]} fps, maximum frame ${m[2]} us, zero late frames`);
}
async function profileGuide(original) {
  let s=await info();if(s.screen_dark){await sim('pwr');await sleep(200)}
  s=await info();if(s.menu){await sim('menu');await sleep(550)}
  await frameWindow('Settings opening',()=>sim('menu'));
  await frameWindow('Settings steady');
  await frameWindow('Guide opening',()=>tap(356,226));
  assert.equal((await info()).agent_view,'guide');
  await frameWindow('Guide next transition',()=>tap(240,428));
  await frameWindow('Guide steady');
  await tap(392,52);preserved(await info(),original);
}
async function main() {
  const original=await until(s=>s.online&&s.key,'initialized paired device');
  if (!original.guide_done && !process.argv.includes('--first-run')) throw Error('Guide is not completed: --first-run explicitly tests and saves initial completion');
  if(process.argv.includes('--first-run')) {
    assert.equal(original.guide_done,false);
    await until(s=>s.online&&s.agent_open&&s.agent_view==='guide','automatic authenticated first-run');
    console.log('PASS first guide opens automatically after authenticated capabilities');
  } else await replay();
  for(let step=1;step<=3;step++) {
    await tap(240,428);const s=await until(s=>s.guide_step===step,'guide step '+step);preserved(s,original);
  }
  await sim('boot');await until(s=>s.guide_step===2,'BOOT returns one guide step');
  await tap(240,428);await tap(240,428);
  const done=await until(s=>s.guide_done&&!s.agent_open&&s.menu,'completion opens real settings');
  preserved(done,original);
  await replay();await sim('boot');
  await until(s=>s.menu&&!s.agent_open,'BOOT at step zero returns to Settings');
  await replay();await tap(240,428);
  await sim('drag',{x:240,y:428,first:true});await sim('drag',{x:240,y:398});
  await sim('tap',{x:240,y:428});assert.equal((await info()).guide_step,1);
  await tap(392,52);await until(s=>!s.menu&&s.guide_done,'skip replay');preserved(await info(),original);
  console.log('PASS four steps, Back, replay, drag cancellation and Skip preserve settings; no uploading capture in idle; Guide capture stays off');
  if(process.argv.includes('--reboot')) {
    const before=await info();let resetSeen=false;
    await control({cmd:'reboot'});
    const restored=await until(s=>{
      if(s.uptime_ms<before.uptime_ms)resetSeen=true;
      return resetSeen&&s.online&&s.key&&s.guide_done;
    },'completion after observed reboot',true);
    await sleep(1000);assert.equal((await info()).agent_open,false);preserved(restored,original);
    console.log('PASS completion survives reboot; guide does not repeat; network/device identity preserved');
  }
  if(process.argv.includes('--profile'))await profileGuide(original);
}
main().catch(e=>{console.error(e.message);process.exitCode=1});
