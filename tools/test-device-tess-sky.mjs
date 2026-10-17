#!/usr/bin/env node
// Runs on the installed ESP32 through its native touch sampler. No provider calls.
// Starts Tess Skies at each depth of the element ladder (ELEMENTS=1,..,10), puts the tools, relays and an aimed source
// where the composed solution has them (a finger or tilt for the skies that need one), and checks that the sky resolves
// into its own melody, that the frames keep their pace while it plays and resolves, and that the speaker is never starved.
import assert from 'node:assert/strict';
import {setTimeout as sleep} from 'node:timers/promises';
import {setDeviceScreen, assertIdleCapture, flushStats} from './test-device/diagnostics.mjs';

const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
const LATE_SHARE = .03;   // of the frames of a sky: those that took longer than their 33 ms
const LATE_FLOOR = 4;     // a short sky of ~120 frames cannot show a share: a few 34-37 ms frames are tolerated
const SOLVE_MS = 40000;
async function control(body) {
  const response = await fetch(`${url}/config`, {method:'POST', headers:{'content-type':'application/json'},
    body:JSON.stringify(body), signal:AbortSignal.timeout(5000)});
  assert.equal(response.status, 200);
  return response.json();
}
const info = () => control({cmd:'info'});
const sim = (ev, fields={}) => control({cmd:'sim', ev, ...fields});
const sky = () => sim('sky-state');
const PHASE = {unravel:1, play:2, resolve:3, fade:4};
async function until(read, check, label, timeout=10000) {
  const end = performance.now()+timeout;
  let value;
  do { value = await read(); if (check(value)) return value; await sleep(60); } while (performance.now()<end);
  throw Error(`Timed out: ${label} (${JSON.stringify(value)})`);
}
// Each depth is a visit of its own: the one still going ends first.
async function endVisit(elements) {
  if (!(await sky()).active) return;
  await sim('boot'); await until(sky, v=>!v.active, `depth ${elements}: the visit ends`, 5000); await sleep(2500);
}
// Brings a sky of this depth to its resolution; returns what it was and how its frames fared.
async function solve(elements, skill) {
  assert.equal((await sim('sky-start',{elements,skill})).ok, true, `a sky starts at depth ${elements}`);
  const first = await until(sky, s=>s.active&&s.phase===PHASE.play&&s.here===0, `depth ${elements}: the stream flows`, 8000);
  await flushStats({sim});
  await sim('sky-solve');
  const end = performance.now()+SOLVE_MS;
  let s = first;
  while (performance.now()<end && s.active && s.phase===PHASE.play) {
    if (first.kind===1) await sim('gravity',{x:first.gx,y:first.gy,ms:600});                              // poured: tilt
    else if (first.tools===0&&first.relays===0&&!first.aim) await sim('pointer',{down:true,x:Math.round(first.sx),y:Math.round(first.sy)});  // nothing to set: a finger
    await sleep(120);
    s = await sky();
  }
  await sim('pointer',{down:false,x:240,y:255}); await sim('gravity',{x:0,y:0,ms:0});
  const played = await flushStats({sim});
  const sang = await until(sky, v=>!v.active||v.phase!==PHASE.resolve||v.sung===v.melody, `depth ${elements}: the melody is sung`, 6000).catch(()=>null);
  await sleep(1800);   // its chord, and the next sky growing
  const resolved = await flushStats({sim});
  await endVisit(elements);
  return {first, phase:s.phase, played, resolved, sung:sang?.sung ?? 0, melody:sang?.melody ?? 0};
}
// BOOT, the menu and PWR each end a sky that is being played, at once.
async function exits() {
  for (const exit of ['boot','menu','pwr']) {
    // (right after a sky was cut short her dots are still coming home and a start is declined: ask again)
    for (let tries=0; tries<12 && !(await sky()).active; tries++) { assert.equal((await sim('sky-start',{elements:3,skill:100})).ok, true); await sleep(1000); }
    await until(sky, s=>s.active&&s.phase===PHASE.play, `${exit}: the stream flows`, 8000);
    await sleep(1500);
    await sim(exit);
    await until(sky, s=>!s.active, `${exit} ends the sky`, 4000);
    if (exit==='menu' && (await info()).menu) await sim('menu');
    if (exit==='pwr') { await sleep(1500); if ((await info()).screen_dark) await sim('pwr'); }
    await sleep(2500);
  }
}
// Rocking the flat cube summons her (her star comes out) and only that; standing, it does nothing.
async function rock(y) {
  for (const x of [.3,-.3,.3]) { await sim('gravity',{x,y,ms:450}); await sleep(380); }
  await sim('gravity',{x:0,y,ms:0}); await sleep(300);
}
async function summons() {
  await until(sky, s=>s.invite===0, 'no invitation yet', 9000);
  await rock(1);
  assert.equal((await sky()).invite, 0, 'rocked while standing: nothing');
  await rock(.1);
  const s = await until(sky, v=>v.invite>0, 'a rock summons her', 3000);
  assert.equal(s.active, false, 'a summon is not the game');
  await sleep(6000);
  await until(sky, v=>v.invite===0, 'ignored, it fades', 3000);
  await rock(.1);                                    // summoned again; lifting the cube answers it like a touch
  await until(sky, v=>v.invite>0, 'summoned again', 3000);
  await sim('gravity',{x:0,y:1,ms:1500});
  await until(sky, v=>v.active, 'raising answers the invitation', 3000);
  await sim('boot'); await until(sky, v=>!v.active, 'the sky ends', 4000); await sleep(3500);
}
async function main() {
  const original = await info(); assertIdleCapture(original);
  if (original.character==='Plush') {
    assert.equal((await sky()).active,false);
    console.log('Plush: Skies inactive; Skies applies to Tess only'); return;
  }
  assert.equal(original.character,'Tess'); assert.ok(!original.screen_lab);
  try {
    if (original.menu) await sim('menu');
    await setDeviceScreen(info,sim,false);
    await sleep(2500);
    await sim('audio-stats',{reset:true});
    const report = [];
    for (const elements of (process.env.ELEMENTS ?? '1,2,3,4,5,6,7,8,9,10').split(',').map(Number)) {
      const r = await solve(elements, 100);
      const frames = r.played.frames + r.resolved.frames, late = r.played.late + r.resolved.late;
      report.push({elements, kind:r.first.kind, stars:r.first.stars, tools:r.first.tools, relays:r.first.relays, aim:r.first.aim, phase:r.phase, sung:`${r.sung}/${r.melody}`, frames, late, lateIn:`${r.played.late}+${r.resolved.late}`,
        maxMs:+Math.max(r.played.maxMs, r.resolved.maxMs).toFixed(1), meanMs:+((r.played.meanMs+r.resolved.meanMs)/2).toFixed(1)});
      console.log(JSON.stringify(report.at(-1)));
      await sim('menu'); await until(info,s=>s.menu,'menu',4000); await sim('menu'); await until(info,s=>!s.menu,'home',4000);
      await until(sky, s=>!s.active, 'the sky ends', 8000);
      await sleep(2500);
    }
    await summons();
    await exits();
    // What was learnt is kept on the device once the sky has settled.
    const kept = await until(sky, s=>s.packed===s.saved, 'the progress is saved', 15000);
    assert.ok(kept.solved>0&&kept.saved>0, 'solved skies are remembered');
    const audio = await sim('audio-stats'); console.log(JSON.stringify(audio));
    for (const row of report) {
      assert.equal(row.phase,PHASE.resolve, `depth ${row.elements}: the sky resolved rather than fading from inactivity`);
      assert.ok(/^(\d+)\/\1$/.test(row.sung)&&row.sung!=='0/0', `depth ${row.elements}: its melody was sung (${row.sung})`);
      assert.ok(row.late <= Math.max(row.frames*LATE_SHARE, LATE_FLOOR), `depth ${row.elements}: ${row.late} of ${row.frames} frames late`);
    }
    assert.equal(audio.late_feeds,0,'speaker feed stays within its reserve');
    assert.equal(audio.write_errors,0,'codec accepts every frame');
    console.log('device skies: every depth resolved and sang, BOOT/menu/PWR end a sky, frames on pace, speaker fed');
  } finally {
    await sim('pointer',{down:false,x:240,y:255}); await sim('gravity',{x:0,y:0,ms:0});
    if ((await info()).menu!==original.menu) await sim('menu');
    await until(sky, s=>!s.active, 'the sky ends', 8000).catch(()=>{});
    assert.equal((await sim('sky-restore')).ok, true, "the player's own progress is put back");
    await setDeviceScreen(info,sim,original.screen_dark);
  }
}
main().catch(error=>{console.error(error.stack);process.exitCode=1;});
