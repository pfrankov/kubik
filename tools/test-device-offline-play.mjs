// Bounded loss of the native Wi-Fi agent route; no saved connection or identity changes.
import assert from 'node:assert/strict';
import {setTimeout as sleep} from 'node:timers/promises';
import {setDeviceScreen, waitForDeviceLink, waitForIdleCapture} from './test-device/diagnostics.mjs';
const url = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function control(body) {
  const response = await fetch(`${url}/config`, {method:'POST', headers:{'content-type':'application/json'},
    body:JSON.stringify(body), signal:AbortSignal.timeout(5000)});
  assert.equal(response.status,200);
  const state = await response.json(); assert.equal(state.ok,true); return state;
}
const info = () => control({cmd:'info'});
const sim = (ev, data={}) => control({cmd:'sim',ev,...data});
async function until(read, check, label, ms=10000) {
  const end = Date.now()+ms;
  do { const state=await read(); if(check(state))return state; await sleep(100); } while(Date.now()<end);
  throw Error(`Timed out: ${label}`);
}
const game = () => sim('game-state');
const sky = () => sim('sky-state');
async function main() {
  const original = await waitForDeviceLink(info);
  assert.equal(original.character,'Tess'); assert.equal(original.screen_lab,false);
  try {
    if(original.menu)await sim('menu');
    await setDeviceScreen(info,sim,false);
    await sim('wifi',{ms:70000});
    await until(info,s=>!s.online,'native agent route disconnected');
    await until(game,s=>!s.available,'ordinary offline points do not hide an interactive cube');
    await sim('boot'); await until(info,s=>s.offline_play,'BOOT enters local play');
    await until(game,s=>s.available,'local games available without the agent');
    await sleep(2500);
    await sim('sky-start',{elements:1,skill:100});
    await until(sky,s=>s.active&&s.phase===2,'offline Skies starts');
    assert.equal((await info()).online,false);
    await sim('boot'); await until(sky,s=>!s.active,'BOOT ends Skies');
    assert.equal((await info()).offline_play,true);
    await sim('boot'); await until(info,s=>!s.offline_play,'next BOOT returns to points');
    await until(game,s=>!s.available,'points remain isolated');
    await sim('boot'); await until(info,s=>s.offline_play,'local play again');
    await setDeviceScreen(info,sim,true); await sim('boot');
    assert.equal((await info()).screen_dark,true,'BOOT cannot wake a manually darkened device');
    await setDeviceScreen(info,sim,false);
    await until(info,s=>s.online&&!s.offline_play,'normal reconnection clears the local override',90000);
    await waitForIdleCapture(info);
    console.log('offline play: explicit BOOT, isolated points, native Skies, dark gate and reconnection passed');
  } finally {
    await until(sky,s=>!s.active,'Skies ended').catch(async()=>sim('boot'));
    await sim('sky-restore');
    await waitForDeviceLink(info,{timeoutMs:90000});
    if((await info()).menu!==original.menu)await sim('menu');
    await setDeviceScreen(info,sim,original.screen_dark);
    const after=await info();
    for(const key of ['device','key','url','ssid','volume','ui_volume','brightness','guide_done'])
      assert.equal(after[key],original[key],`${key} preserved`);
  }
}
main().catch(error=>{console.error(error.stack);process.exitCode=1});
