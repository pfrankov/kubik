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
  await tap(); await sleep(700); await tap(180,220);
  await until(state,s=>s.game===1,'ordinary tap and invitation start Duet');
}
async function swipeInvite(direction=0) {
  const [dx,dy]=[[1,0],[-1,0],[0,1],[0,-1]][direction];
  for(let n=0;n<=8;n++) {
    await pointer(true,240+n*13*dx,255+n*13*dy); await sleep(25);
  }
  await pointer(false,240+104*dx,255+104*dy);
  await until(state,s=>s.game===2,'ordinary directional swipe invites Chase');
}
async function winDuet(tier) {
  let current=await until(state,s=>s.game===1&&s.phase===2,'Duet answer window');
  for(let step=current.step;step<4+2*tier;step++) {
    await sleep(120+(step%3)*130);
    await tap(step%2?180:300,step%3?220:300);
    current=await until(state,s=>s.game===1&&s.step===step+1&&s.phase!==1,'Tess answers the player');
  }
  const result=await until(state,s=>s.game===1&&s.phase===3,'Duet celebration');
  assert.ok(result.progress&(1<<tier));
}
async function winChase(tier) {
  await until(state,s=>s.game===2&&s.phase===2&&s.hit_valid,'Chase rendered target');
  let previous, last=0;
  for(let n=0;n<tier+3;n++) {
    const hit=await until(state,s=>s.game===2&&s.phase===2&&s.hit_valid&&performance.now()-last>=750&&
      (!previous||Math.hypot(s.hit_x-previous[0],s.hit_y-previous[1])>=55),'Chase target moved');
    previous=[hit.hit_x,hit.hit_y]; last=performance.now();
    await tap(...previous);
    await until(state,s=>s.step===n+1&&s.game===2,'native Chase hit');
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
  const name=index===22?'Growth: point':`${index<29?'Duet':'Chase'}: tier ${(index<29?index-26:index-29)+1}`;
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
  await echoInvite(); await winDuet(0); await catalog();
  for(const direction of [0,1,2,3]) {
    await select(22); await swipeInvite(direction); await winChase(0); await catalog();
  }
  for(let game=0;game<2;game++) for(let tier=0;tier<3;tier++) {
    await select((game?29:26)+tier);
    if(game) await winChase(tier); else await winDuet(tier);
    assert.equal((await state()).saved,saved,'Lab must not persist victories');
    await catalog();
  }
  await select(29); await until(state,s=>s.phase===2,'Chase miss window');
  const missedProgress=(await state()).progress;
  await tap(10,10); await sleep(150);
  const missed=await state();
  assert.equal(missed.game,2,'a miss stays playful'); assert.equal(missed.step,0);
  assert.equal(missed.progress,missedProgress); await catalog();
  await select(26); await until(state,s=>s.phase===2,'Duet first turn');
  const beforeRapid=(await state()).progress;
  await tap(); await sleep(80); await tap();
  const rapid=await state(); assert.equal(rapid.game,1); assert.equal(rapid.step,1);
  assert.equal(rapid.progress,beforeRapid); await catalog();
  await select(26);
  await pointer(true); await sleep(100); await pointer(true,275,255); await sleep(100);
  await pointer(false,350,255);
  await until(state,s=>s.game===0,'drag yields to normal rotation',1000);
  await sleep(750); assert.equal((await state()).game,0,'exit drag cannot invite another game');
  await catalog();
  await select(26);
  for(let n=0;n<4;n++) { await pointer(true); await sleep(200); }
  await pointer(false);
  await until(state,s=>s.game===0,'held finger ends game',1000); await catalog();
  await select(26); await until(state,s=>s.game===1&&s.phase===2,'unanswered Duet window');
  const waitingAt=performance.now();
  await until(state,s=>s.game===0,'unanswered game timeout',8000);
  assert.ok(performance.now()-waitingAt>=4500,'timeout must not be immediate'); await catalog();
  await sim('boot'); await until(info,s=>!s.screen_lab&&s.menu,'real Settings');
  await sim('menu'); await until(info,s=>!s.menu,'home');
  await sleep(600); // menu.open clears before its visual transition finishes
}
async function realDiscoveries() {
  for(let game=0;game<2;game++) for(let tier=0;tier<3;tier++) {
    const before=await state(), bit=1<<(game*3+tier);
    if(before.progress&bit) continue;
    if(game) await swipeInvite(tier); else await echoInvite();
    assert.equal((await state()).tier,tier);
    if(game) await winChase(tier); else await winDuet(tier);
    const expected=before.progress|bit;
    await until(state,s=>s.saved===expected,'milestone committed to NVS');
    await until(state,s=>s.game===0,'celebration ends');
    console.log(`native ${game?'Chase':'Duet'} tier ${tier+1}: durable mask ${expected}`);
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
  const original=await info(); assertIdleCapture(original);
  if(original.character==='Plush') {
    const inactive=await state(); assert.equal(inactive.game,0); assert.equal(inactive.available,false);
    console.log('Plush: Tess games inactive; game-play tests apply to Tess only'); return;
  }
  assert.equal(original.character,'Tess'); assert.ok(!original.screen_lab);
  const saved=(await state()).saved;
  try {
    if(original.menu) await sim('menu');
    await setDeviceScreen(info,sim,false);
    await waitForDeviceLink(info); // sleeping radio checks can be a minute apart
    await temporaryGames(saved);
    assert.equal((await state()).saved,saved);
    if(learn) await realDiscoveries();
    const after=await info();
    for(const field of ['ssid','url','key','guide_done','volume','ui_volume','brightness'])
      assert.equal(after[field],original[field],`${field} changed`);
    assert.equal(after.heap_failures,original.heap_failures);
    console.log('device games: real touch sampling, four swipe directions, six tiers, varied pace, misses, drag, hold, timeout and isolated Lab passed');
  } finally {
    await pointer(false);
    if((await info()).screen_lab) await sim('menu');
    if((await info()).menu!==original.menu) await sim('menu');
    await setDeviceScreen(info,sim,original.screen_dark);
  }
}
main().catch(error=>{console.error(error.stack);process.exitCode=1;});
