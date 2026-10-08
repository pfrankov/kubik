#!/usr/bin/env node
// Runs on the installed ESP32 through its native touch sampler. No provider calls.
// Default: temporary Screen Lab progress. --learn earns missing real discoveries.
import assert from 'node:assert/strict';
import {setTimeout as sleep} from 'node:timers/promises';
import {waitForDeviceLink, setDeviceScreen, assertIdleCapture} from './test-device/diagnostics.mjs';

const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
const learn = process.argv.includes('--learn');
let labPage = 0;
async function control(body) {
  const response = await fetch(`${url}/config`, {method:'POST', headers:{'content-type':'application/json'},
    body:JSON.stringify(body), signal:AbortSignal.timeout(5000)});
  assert.equal(response.status, 200);
  const state = await response.json(); assert.equal(state.ok, true); return state;
}
const info = () => control({cmd:'info'});
const sim = (ev, fields={}) => control({cmd:'sim', ev, ...fields});
const state = () => sim('game-state');
const pointer = (down,x=240,y=255) => sim('pointer',{down,x:Math.round(x),y:Math.round(y)});
async function until(read, check, label, timeout=10000) {
  const end = performance.now()+timeout;
  do { const value = await read(); if (check(value)) return value; await sleep(50); } while (performance.now()<end);
  throw Error(`Timed out: ${label}`);
}
async function tap(x=240,y=255) {
  await pointer(true,x,y); await sleep(70); await pointer(false,x,y); await sleep(90);
}
async function echoInvite() {
  for(let n=0;n<3;n++) { const at=performance.now(); await tap(); await sleep(Math.max(0,300-(performance.now()-at))); }
  await until(state,s=>s.game===1,'native triple tap invites Echo');
}
async function circleInvite(direction=1) {
  for(let n=0;n<=40;n++) {
    const angle=direction*n*Math.PI*2/40;
    await pointer(true,240+100*Math.cos(angle),255+100*Math.sin(angle)); await sleep(45);
  }
  await pointer(false,340,255);
  await until(state,s=>s.game===2,'native closed circle invites Catch');
}
async function winEcho(tier) {
  await until(state,s=>s.game===1&&s.phase===2,'Echo answer window');
  const gaps=[[600],[450,750],[450,450,900]][tier];
  await tap();
  for(const gap of gaps) { await sleep(gap-160); await tap(); }
  const result=await until(state,s=>s.game===1&&s.phase===3,'Echo celebration');
  assert.ok(result.progress&(1<<tier));
}
async function winCatch(tier) {
  await until(state,s=>s.game===2&&s.phase===2&&s.hit_valid,'Catch rendered target');
  let previous, last=0;
  for(let n=0;n<tier+3;n++) {
    const hit=await until(state,s=>s.game===2&&s.phase===2&&s.hit_valid&&performance.now()-last>=750&&
      (!previous||Math.hypot(s.hit_x-previous[0],s.hit_y-previous[1])>=55),'Catch target moved');
    previous=[hit.hit_x,hit.hit_y]; last=performance.now();
    await tap(...previous);
    await until(state,s=>s.step===n+1&&s.game===2,'native Catch hit');
  }
  const result=await state(); assert.equal(result.phase,3); assert.ok(result.progress&(1<<(tier+3)));
}
async function catalog() {
  await sim('boot'); await until(info,s=>s.lab_screen==='Screen Lab','Lab catalog');
}
async function select(index) {
  const target=Math.floor(index/3), turns=(target-labPage+11)%11;
  for(let page=0;page<turns;page++) { await sim('tap',{x:356,y:428}); await sleep(90); }
  labPage=target;
  await sim('tap',{x:240,y:144+(index%3)*88});
  const name=index===22?'Growth: point':`${index<29?'Echo':'Catch'}: tier ${(index<29?index-26:index-29)+1}`;
  await until(info,s=>s.lab_screen===name,'Lab preview');
}
async function openLab() {
  await sim('menu'); await until(info,s=>s.menu,'Settings'); await sleep(600);
  for(let n=0;n<5;n++) { await sim('tap',{x:90,y:36}); await sleep(90); }
  await sim('hold',{x:90,y:36}); await until(info,s=>s.screen_lab,'hidden Lab entry');
  labPage=0;
}
async function temporaryGames(saved) {
  await openLab();
  await select(22); await until(state,s=>s.available,'Growth preview');
  await echoInvite(); await winEcho(0); await catalog();
  for(const direction of [-1,1]) {
    await select(22); await circleInvite(direction); await winCatch(0); await catalog();
  }
  for(let game=0;game<2;game++) for(let tier=0;tier<3;tier++) {
    await select((game?29:26)+tier);
    if(game) await winCatch(tier); else await winEcho(tier);
    assert.equal((await state()).saved,saved,'Lab must not persist victories');
    await catalog();
  }
  await select(29); await until(state,s=>s.phase===2,'Catch miss window');
  const missedProgress=(await state()).progress;
  await tap(10,10); await until(state,s=>s.game===0,'miss ends Catch',1000);
  assert.equal((await state()).progress,missedProgress); await catalog();
  await select(26); await until(state,s=>s.phase===2,'Echo wrong rhythm window');
  const wrongProgress=(await state()).progress;
  await tap(); await sleep(150); await tap(); await until(state,s=>s.game===0,'wrong rhythm ends Echo',1000);
  assert.equal((await state()).progress,wrongProgress); await catalog();
  await select(26);
  for(let n=0;n<4;n++) { await pointer(true); await sleep(200); }
  await pointer(false);
  await until(state,s=>s.game===0,'held finger ends game',1000); await catalog();
  await select(26); await until(state,s=>s.game===1&&s.phase===2,'unanswered Echo window');
  const waitingAt=performance.now();
  await until(state,s=>s.game===0,'unanswered game timeout',7000);
  assert.ok(performance.now()-waitingAt>=4000,'timeout must not be immediate'); await catalog();
  await sim('boot'); await until(info,s=>!s.screen_lab&&s.menu,'real Settings');
  await sim('menu'); await until(info,s=>!s.menu,'home');
  await sleep(600); // menu.open clears before its visual transition finishes
}
async function realDiscoveries() {
  for(let game=0;game<2;game++) for(let tier=0;tier<3;tier++) {
    const before=await state(), bit=1<<(game*3+tier);
    if(before.progress&bit) continue;
    if(game) await circleInvite(tier%2?-1:1); else await echoInvite();
    assert.equal((await state()).tier,tier);
    if(game) await winCatch(tier); else await winEcho(tier);
    const expected=before.progress|bit;
    await until(state,s=>s.saved===expected,'milestone committed to NVS');
    await until(state,s=>s.game===0,'celebration ends');
    console.log(`native ${game?'Catch':'Echo'} tier ${tier+1}: durable mask ${expected}`);
  }
  assert.equal((await state()).saved,63);
  await until(state,s=>s.form===3,'earned tesseract');
  const uptime=(await info()).uptime_ms;
  await control({cmd:'reboot'});
  await until(info,s=>s.uptime_ms<uptime,'actual reboot',15000);
  await waitForDeviceLink(info);
  assert.equal((await state()).saved,63); assert.equal((await state()).progress,63);
  console.log('six real discoveries survive reboot; earned tesseract restored');
}
async function main() {
  const original=await waitForDeviceLink(info); assertIdleCapture(original);
  if(original.character==='Plush') {
    const inactive=await state(); assert.equal(inactive.game,0); assert.equal(inactive.available,false);
    console.log('Plush: Tess games inactive; game-play tests apply to Tess only'); return;
  }
  assert.equal(original.character,'Tess'); assert.ok(!original.screen_lab);
  const saved=(await state()).saved;
  try {
    if(original.menu) await sim('menu');
    await setDeviceScreen(info,sim,false);
    await temporaryGames(saved);
    assert.equal((await state()).saved,saved);
    if(learn) await realDiscoveries();
    const after=await info();
    for(const field of ['ssid','url','key','guide_done','volume','ui_volume','brightness'])
      assert.equal(after[field],original[field],`${field} changed`);
    assert.equal(after.heap_failures,original.heap_failures);
    console.log('device games: real touch sampling, both circle directions, six tiers, mistakes, hold, timeout and isolated Lab passed');
  } finally {
    await pointer(false);
    if((await info()).screen_lab) await sim('menu');
    if((await info()).menu!==original.menu) await sim('menu');
    await setDeviceScreen(info,sim,original.screen_dark);
  }
}
main().catch(error=>{console.error(error.stack);process.exitCode=1;});
