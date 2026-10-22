#!/usr/bin/env node
// Runs on the installed ESP32 through its native touch sampler. No provider calls.
// Plays every behaviour of Tess on the home screen and in isolated Screen Lab previews.
import assert from 'node:assert/strict';
import {setTimeout as sleep} from 'node:timers/promises';
import {waitForDeviceLink, setDeviceScreen, assertIdleCapture} from './test-device/diagnostics.mjs';

const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
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
async function catalog() {
  await sim('boot'); await until(info,s=>s.lab_screen==='Screen Lab','Lab catalog');
}
async function select(index) {
  const target=Math.floor(index/3), turns=(target-labPage+11)%11;
  for(let page=0;page<turns;page++) { await sim('tap',{x:356,y:428}); await sleep(90); }
  labPage=target;
  await sim('tap',{x:240,y:144+(index%3)*88});
  const names={22:'Growth: point',26:'Play: pluck',27:'Play: bloom',28:'Play: stretch',
    29:'Play: slingshot',30:'Play: gather',31:'Play: alone'};
  const name=names[index];
  await until(info,s=>s.lab_screen===name,'Lab preview');
}
async function openLab() {
  await sim('menu'); await until(info,s=>s.menu,'Settings'); await sleep(600);
  for(let n=0;n<5;n++) { await sim('tap',{x:90,y:36}); await sleep(90); }
  await sim('hold',{x:90,y:36}); await until(info,s=>s.screen_lab,'hidden Lab entry');
  labPage=0;
}
// A finger held on the glass: USB samples expire after 350 ms, so a held finger is refreshed.
async function hold(x,y,ms) { const end=performance.now()+ms; do { await pointer(true,x,y); await sleep(60); } while (performance.now()<end); }
async function pull(fromX,toX,y=255) {
  for(let x=fromX;x<=toX;x+=10) { await pointer(true,x,y); await sleep(40); }
  await hold(toX,y,450);
  const held=await state();
  await pointer(false,toX,y);
  return held;
}
// Watch the play for a while; returns the strongest of each quantity seen.
async function watch(ms) {
  const seen={waves:0,bloom:0,pull:0,speed:0,squash:0,spin:0,shaping:false,games:new Set(),burst:0};
  const end=performance.now()+ms;
  while(performance.now()<end) {
    const s=await state();
    for(const k of ['waves','bloom','pull','speed','squash','spin','burst']) seen[k]=Math.max(seen[k],s[k]);
    seen.shaping=seen.shaping||s.shaping; seen.games.add(s.game);
    await sleep(30);
  }
  return seen;
}
async function sweep() {
  let last=await state();
  for(let row=0,y=24;y<=456&&(!row||(last.game===3&&last.phase!==3));y+=56,row++) {
    for(let x=24;x<=456;x+=36) {
      const s=await pointer(true,row%2?480-x:x,y);
      if(!s.ok) throw Error('pointer sample rejected');
      await sleep(25);  // a quick real finger, not a path faster than the display can sample
    }
    last=await state();
  }
  await pointer(false,240,255);
  return last;
}
async function previews(saved) {
  await openLab();
  const expect={26:s=>s.waves>0,27:s=>s.bloom>.5,28:s=>s.pull>.5,29:s=>s.speed>5,30:s=>s.games.has(3),31:s=>s.spin>0||s.waves>0};
  for(const index of [26,27,28,29,30,31]) {
    await select(index);
    const seen=await watch(1500);
    assert.ok(expect[index](seen)&&seen.shaping|| index===30&&expect[index](seen),`Lab preview ${index} plays`);
    assert.equal((await state()).saved,saved,'Lab must not persist discoveries');
    await catalog();
  }
  await sim('boot'); await until(info,s=>!s.screen_lab&&s.menu,'real Settings');
  await sim('menu'); await until(info,s=>!s.menu,'home');
  await sleep(1500);
}
const TONE={ring:1,creak:2,twang:4,thud:8,grain:16,whoosh:32,chirp:64};
async function tilt(x,y,ms) { assert.equal((await sim('gravity',{x,y,ms})).ok,true); }
// The gyroscope, her temper and her voice, on the device.
async function feelings() {
  await sleep(2000);
  // Tilt: she rolls downhill, knocks into the rim, and rolls home when the device is level again.
  await tilt(.65,0,2500);
  let seen=await watch(2000), far=0;
  for(let i=0;i<10;i++) far=Math.max(far,(await state()).x);
  const rolled=await state();
  assert.ok(rolled.tilt_x>.3,'the probe tilt reaches her');
  assert.ok(far>330&&seen.squash>.05,`a tilt rolls her into the rim (x ${far}, squash ${seen.squash})`);
  await until(state,s=>Math.abs(s.x-240)<15,'level again, she rolls home',8000);
  // Pour: a burst held tilted slides downhill; tilting back and forth pours dots through her core.
  await sleep(1500);
  for(let n=0;n<6;n++) { await tap(240,255); await sleep(120); }
  await until(state,s=>s.burst>.8,'a burst',3000);
  for(let swing=0;swing<6;swing++) { await tilt(swing%2?-.7:.7, swing%3?-.4:.5, 1100); await sleep(1000); }
  const poured=await state();
  assert.ok(poured.streak>3||poured.phase===3,`tilting pours dots home (${poured.streak})`);
  await tilt(0,0,0);
  await until(state,s=>s.burst===0,'the burst ends',12000);
  // Temper: poked again and again in one place, she gets cross, and says so.
  await sleep(1500);
  let cross=0;
  for(let n=0;n<10;n++) { await tap(300,255); cross=Math.max(cross,(await state()).annoy); await sleep(80); }
  const voiced=await state();
  assert.ok(cross>.5,`rapid pokes annoy her (${cross})`);
  assert.ok((voiced.tones&(TONE.ring|TONE.chirp|TONE.thud|TONE.grain))===(TONE.ring|TONE.chirp|TONE.thud|TONE.grain),`her body sounds (${voiced.tones})`);
  await until(state,s=>s.annoy<.3,'and it passes',20000);
}
async function realPlay() {
  // Pluck: a tap on her rings a wave through her edges; two quick taps make two.
  await tap(265,255);
  let seen=await watch(500); assert.ok(seen.waves>=1&&seen.shaping,'a tap plucks her');
  await tap(205,205); await tap(285,305);
  seen=await watch(300); assert.ok(seen.waves>=2,'two plucks ring together');
  await sleep(2000);
  // Bloom: a run of swipes across her winds her up until she opens.
  // (each lift lasts longer than a display frame, as a real finger's does)
  let opened=0;
  for(let n=0;n<12&&opened<.5;n++) {
    for(let x=170;x<=330;x+=40) { await pointer(true,x,255); await sleep(15); }
    await pointer(false,370,255); await sleep(150);
    opened=Math.max(opened,(await state()).bloom);
  }
  seen=await watch(600); assert.ok(Math.max(opened,seen.bloom)>.5,`swipes open her (bloom ${Math.max(opened,seen.bloom)})`);
  await until(state,s=>s.bloom<.2,'she closes as the spin fades',8000);
  // Stretch and twang: beside her, a short pull.
  let held=await pull(380,395); assert.equal(held.game,1,'a finger beside her stretches her'); assert.ok(held.pull>.9);
  await until(state,s=>s.game===0&&s.pull===0,'the twang lets go',4000);  // (excited, she may soon play on by herself)
  // Slingshot: a long pull flies her into the rim and she bounces home.
  held=await pull(390,440); assert.equal(held.game,1);
  seen=await watch(2500); assert.ok(seen.games.has(2)&&seen.speed>5&&seen.squash>.1,'a long pull slings her into the rim');
  await until(state,s=>s.game===0,'she comes home',6000);
  // Priority: Settings takes her out of a pull at once.
  await pointer(true,390,255); await sleep(100); await pointer(true,420,255);
  await until(state,s=>s.game===1,'a pull to interrupt',1000);
  await sim('menu'); await until(state,s=>s.game===0&&s.pull===0,'Settings stops the pull',1000);
  await pointer(false,420,255); await sim('menu'); await until(info,s=>!s.menu,'home'); await sleep(1500);
  // Shatter and gather: the hardest throw bursts her; a sweep brings every dot home.
  await pull(390,472);
  await until(state,s=>s.burst>0,'a hard landing shatters her',3000);
  await sleep(700);
  const done=await sweep();
  assert.equal(done.phase,3,'every dot came home');
  await until(state,s=>s.game===0,'the gather ends',4000);
  await feelings();
  console.log('device play: pluck, echo, bloom, twang, slingshot, settings priority, shatter, gather, tilt, pour and temper passed');
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
    await previews(saved);
    assert.equal((await state()).saved,saved);
    if(process.argv.includes("--learn")) await realPlay();
    else assert.equal((await state()).saved,saved,"Lab acceptance preserves discoveries");
    const after=await info();
    for(const field of ['ssid','url','key','guide_done','volume','ui_volume','brightness'])
      assert.equal(after[field],original[field],`${field} changed`);
    assert.equal(after.heap_failures,original.heap_failures);
    console.log('device games: isolated Lab and unchanged settings passed; persistent home play requires --learn');
  } finally {
    await pointer(false);
    if((await info()).screen_lab) await sim('menu');
    if((await info()).menu!==original.menu) await sim('menu');
    await setDeviceScreen(info,sim,original.screen_dark);
  }
}
main().catch(error=>{console.error(error.stack);process.exitCode=1;});
