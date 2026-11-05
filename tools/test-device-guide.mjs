#!/usr/bin/env node
// Physical guide replay/completion. --first-run requires the automatic first welcome tour.
// --reboot proves saved completion survives reboot; --profile measures native Settings/guide frame windows.
// No voice/inference calls.
import assert from 'node:assert/strict';
import {deviceGeometry} from './test-device/geometry.mjs';
import { pathToFileURL } from 'node:url';
import { assertIdleCapture, flushStats, waitForIdleCapture } from './test-device/diagnostics.mjs';
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
export function isProfileScreen(state, view, step) {
  const unobstructed = ['screen_dark', 'setup_ap', 'screen_lab', 'status_open', 'sound_menu', 'card_open', 'pair_visible']
    .every(key => !state[key]);
  if (!unobstructed) return false;
  if (view === 'home') return state.menu === false && state.agent_open === false;
  if (view === 'settings') return state.menu === true && state.agent_open === false;
  return view === 'guide' && state.menu === true && state.agent_open === true && state.agent_view === 'guide' &&
    (step === undefined || state.guide_step === step);
}
export async function prepareHome({ read = info, event = sim, wait = until } = {}) {
  if ((await read()).screen_dark) {
    await event('pwr'); await wait(s => !s.screen_dark, 'awake screen before guide');
  }
  // A blank saved SSID deliberately reopens setup after reboot; online is not proof of home.
  if ((await read()).setup_ap) {
    await event('boot'); await wait(s => !s.setup_ap, 'BOOT closes initial setup before guide');
  }
  if ((await read()).menu) {
    await event('menu'); await wait(s => !s.menu && !s.agent_open, 'menu closed before guide');
  }
  return wait(s => isProfileScreen(s, 'home'), 'unobstructed home before guide');
}
async function replay(g) {
  await prepareHome();
  await sim('menu');await until(s => isProfileScreen(s, 'settings'), 'Settings before guide replay');
  await sleep(550);await tap(356,g.settingsGuide);
  return until(s=>isProfileScreen(s, 'guide', 0),'replayed guide');
}
async function frameWindow(label, view, action, before = view, step) {
  assert.ok(isProfileScreen(await info(), before), `${label}: unexpected screen before measurement`);
  await flushStats({sim});if(action)await action();
  await until(s => isProfileScreen(s, view, step), `${label}: expected ${view} screen`);
  await sleep(2300);
  assert.ok(isProfileScreen(await info(), view, step), `${label}: screen changed during measurement`);
  const sample=await flushStats({sim});
  console.log(`PROFILE ${label}: ${JSON.stringify(sample)}`);
  assert.equal(sample.presented,sample.frames,`${label}: unpresented frames`);
  assert.equal(sample.overflows,0,`${label}: frame storage overflow`);
  assert.ok(sample.fps>=29.5,`${label}: ${sample.fps} fps`);
  assert.equal(sample.late,0,`${label}: ${sample.late} late frames, maximum ${sample.maxMs} ms (limit 33.333 ms)`);
  console.log(`PASS ${label}: ${sample.fps} fps, maximum frame ${sample.maxMs} ms, zero late frames`);
}
async function profileGuide(original,g) {
  await prepareHome();
  await frameWindow('Settings opening', 'settings', ()=>sim('menu'), 'home');
  await frameWindow('Settings steady', 'settings');
  await frameWindow('Guide opening', 'guide', ()=>tap(356,g.settingsGuide), 'settings', 0);
  await frameWindow('Guide next transition', 'guide', ()=>tap(240,g.footer), 'guide', 1);
  await frameWindow('Guide steady', 'guide', undefined, 'guide', 1);
  await tap(392,52);preserved(await info(),original);
}
async function main() {
  const original=await until(s=>s.online&&s.key,'initialized paired device');
  const g=deviceGeometry(original);
  if (!original.guide_done && !process.argv.includes('--first-run')) throw Error('Guide is not completed: --first-run explicitly tests and saves initial completion');
  if(process.argv.includes('--first-run')) {
    assert.equal(original.guide_done,false);
    await until(s=>s.online&&s.agent_open&&s.agent_view==='guide','automatic authenticated first-run');
    console.log('PASS first guide opens automatically after authenticated capabilities');
  } else await replay(g);
  for(let step=1;step<=3;step++) {
    await tap(240,g.footer);const s=await until(s=>s.guide_step===step,'guide step '+step);preserved(s,original);
  }
  await sim('boot');await until(s=>s.guide_step===2,'BOOT returns one guide step');
  await tap(240,g.footer);await tap(240,g.footer);
  const done=await until(s=>s.guide_done&&!s.agent_open&&s.menu,'completion opens real settings');
  preserved(done,original);
  await replay(g);await sim('boot');
  await until(s=>s.menu&&!s.agent_open,'BOOT at step zero returns to Settings');
  await replay(g);await tap(240,g.footer);
  await sim('drag',{x:240,y:g.footer,first:true});await sim('drag',{x:240,y:g.footer-30});
  await sim('tap',{x:240,y:g.footer});assert.equal((await info()).guide_step,1);
  await tap(392,52);await until(s=>!s.menu&&s.guide_done,'skip replay');preserved(await info(),original);
  console.log('PASS four steps, Back, replay, drag cancellation and Skip preserve settings; no uploading capture in idle; Guide capture stays off');
  if(process.argv.includes('--reboot')) {
    const before=await info();let resetSeen=false;
    await control({cmd:'reboot'});
    await until(s=>{
      if(s.uptime_ms<before.uptime_ms)resetSeen=true;
      return resetSeen&&s.online&&s.key&&s.guide_done;
    },'completion after observed reboot',true);
    await sleep(1000);assert.equal((await info()).agent_open,false);
    preserved(await waitForIdleCapture(info),original);
    console.log('PASS completion survives reboot; guide does not repeat; network/device identity preserved');
  }
  if(process.argv.includes('--profile'))await profileGuide(original,g);
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  main().catch(e=>{console.error(e.message);process.exitCode=1});
