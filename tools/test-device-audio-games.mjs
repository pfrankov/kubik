// Local native touch workload in isolated Screen Lab; no provider calls or saved-setting changes.
import assert from 'node:assert/strict';
import {setTimeout as sleep} from 'node:timers/promises';
import {setDeviceScreen, waitForDeviceLink, assertIdleCapture} from './test-device/diagnostics.mjs';

const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const r = await fetch(`${url}/config`, {method:'POST', headers:{'content-type':'application/json'},
    body:JSON.stringify(body), signal:AbortSignal.timeout(5000)});
  assert.equal(r.status,200); const s=await r.json(); assert.equal(s.ok,true); return s;
}
const info=()=>control({cmd:'info'});
const sim=(ev,fields={})=>control({cmd:'sim',ev,...fields});
const pointer=(down,x,y)=>sim('pointer',{down,x,y});
async function tap(x,y) { await pointer(true,x,y); await sleep(70); await pointer(false,x,y); await sleep(90); }
async function exercise() {
  for(let round=0;round<3;round++) {
    for(let n=0;n<8;n++) { await tap(240,255); await sleep(100); }
    await sleep(800);
    for(let n=0;n<8;n++) {
      for(let x=170;x<=330;x+=40) { await pointer(true,x,255); await sleep(20); }
      await pointer(false,370,255); await sleep(140);
    }
    await sleep(600);
    for(let x=390;x<=470;x+=10) { await pointer(true,x,255); await sleep(50); }
    await pointer(false,470,255); await sleep(1500);
    await sim('gravity',{x:round%2?.7:-.7,y:.5,ms:1500}); await sleep(1800);
  }
  await sleep(1500);
}
async function main() {
  await waitForDeviceLink(info);
  const original=await info(); assertIdleCapture(original);
  assert.equal(original.character,'Tess'); assert.equal(original.screen_lab,false);
  assert.equal(original.card_open,false); assert.equal(original.live_active,false);
  try {
    if(original.menu) await sim('menu');
    await setDeviceScreen(info,sim,false); await waitForDeviceLink(info); await sleep(3000);
    await sim('menu'); await sleep(600);
    for(let n=0;n<5;n++) { await sim('tap',{x:90,y:36}); await sleep(90); }
    await sim('hold',{x:90,y:36}); assert.ok((await info()).screen_lab);
    for(let n=0;n<8;n++) { await sim('tap',{x:356,y:428}); await sleep(90); }
    await sim('tap',{x:240,y:320}); await sleep(600);
    assert.equal((await info()).lab_screen,'Play: pluck');
    await sim('audio-stats',{reset:true});
    await exercise();
    const result=await sim('audio-stats'); console.log(JSON.stringify(result));
    assert.ok(result.audible_frames>=100,'The real speaker path must mix sound during this workload');
    if(!process.argv.includes('--measure-only')) {
      assert.equal(result.late_feeds,0,'Speaker feed exceeded its 60ms DMA reserve');
      assert.equal(result.write_errors,0,'Codec rejected an output frame');
      assert.ok(result.mix_max_us<20000,'One mix must fit its 20ms frame');
    }
    const after=await info();
    for(const k of ['ssid','url','key','volume','ui_volume','brightness','guide_done']) {
      assert.ok(after[k]===original[k],`${k} must stay unchanged`);
    }
    assert.equal(after.heap_failures,original.heap_failures);
  } finally {
    await pointer(false,240,255); await sim('gravity',{x:0,y:0,ms:0});
    if((await info()).screen_lab) await sim('menu');
    if((await info()).menu!==original.menu) await sim('menu');
    await setDeviceScreen(info,sim,original.screen_dark);
  }
}
main().catch(e=>{console.error(e.stack);process.exitCode=1;});
