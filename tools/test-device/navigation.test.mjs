import assert from 'node:assert/strict';
import test from 'node:test';
import { homeBootProof, restoreNavigationState } from '../test-device-navigation.mjs';

const original = () => ({ chip:'esp32s3', voice_mode:'classic', stt_known:true, stt_available:true,
  ssid:'', url:'', key:'public-key', volume:42, brightness:180, character:'Tess', guide_done:true,
  menu:false, screen_dark:false, online:true, mic_open:false, mic_enabled:false, mic_rx_enabled:false,
  mic_reads:10, mic_idle_reads:0, wake_listening:false, auto_recording:false, live_active:false });
function bootFixture({ earlyEnd = false } = {}) {
  const state = original(), events = [], pauses = [];
  let presses = 0, windows = 0;
  return { state, events, pauses,
    hooks: {
      read: async () => ({...state}),
      event: async ev => {
        events.push(ev); assert.equal(ev,'boot'); presses++;
        state.mic_open = state.mic_enabled = state.mic_rx_enabled = presses === 1;
      },
      wait: async (check,label) => { assert.ok(check(state),label);return {...state}; },
      pause: async ms => {
        pauses.push(ms);
        if (ms === 600) { state.mic_reads += 30; if(earlyEnd)state.mic_open=false; }
      },
      stats: async () => ({ modes: ++windows > 2 ? 1 << 1 : 1 << 3 }),
    },
  };
}
test('S3 navigation observes latched reads, sends once, and waits for a displayed idle reply', async () => {
  const fixture = bootFixture();
  await homeBootProof(original(),fixture.hooks);
  assert.deepEqual(fixture.events,['boot','boot']);
  assert.equal(fixture.pauses[0],600);
  assert.equal(fixture.pauses.filter(ms=>ms===350).length,2);
  assert.equal(fixture.state.mic_open,false);
});
test('S3 navigation rejects a short-press recording that ended during startup', async () => {
  const fixture=bootFixture({earlyEnd:true});
  await assert.rejects(homeBootProof(original(),fixture.hooks),/did not latch/);
  assert.deepEqual(fixture.events,['boot']);
});
test('C6 navigation keeps the original non-voice main BOOT proof', async () => {
  const fixture=bootFixture(), before={...original(),chip:'esp32c6'};
  fixture.hooks.event=async ev=>fixture.events.push(ev);
  await homeBootProof(before,fixture.hooks);
  assert.deepEqual(fixture.events,['boot']);
  assert.deepEqual(fixture.pauses,[200]);
});
function restorationFixture(before, { lostSetup = false } = {}) {
  const state={...original(),menu:true,screen_dark:true}, events=[];
  return {state,events,hooks:{
    read:async()=>({...state}),
    event:async(ev)=>{
      events.push(ev);
      if(ev==='pwr')state.screen_dark=!state.screen_dark;
      if(ev==='menu')state.menu=!state.menu;
      if(ev==='boot')delete state.setup_ap;
      if(ev==='setup'&&!lostSetup)state.setup_ap=before.setup_ap;
    },
    command:async body=>assert.deepEqual(body,{cmd:'card',text:''}),
    wait:async(check,label)=>{assert.ok(check(state),label);return {...state};},
    pause:async()=>{},
  }};
}
test('navigation restoration returns the blank-SSID setup AP and original dark screen',async()=>{
  const before={...original(),setup_ap:'KUBIK-TEST',screen_dark:true};
  const fixture=restorationFixture(before);
  await restoreNavigationState(before,fixture.hooks);
  assert.deepEqual(fixture.events,['pair','pwr','menu','setup','pwr']);
  assert.equal(fixture.state.setup_ap,'KUBIK-TEST');
  assert.equal(fixture.state.screen_dark,true);
});
test('navigation restoration fails if the original setup AP did not return',async()=>{
  const before={...original(),setup_ap:'KUBIK-TEST'}, fixture=restorationFixture(before,{lostSetup:true});
  await assert.rejects(restoreNavigationState(before,fixture.hooks),/restore original setup AP/);
});
