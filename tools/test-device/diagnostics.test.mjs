import assert from 'node:assert/strict';
import test from 'node:test';
import { flushStats, measureWindow, MissingStats, setDeviceScreen, waitForDeviceLink, waitForIdleCapture } from './diagnostics.mjs';
import { assertPlaybackWindow, assertPlaybackCompletion, assertFaultWindow, PROFILE_MS, speechProfile } from './speech-profile.mjs';
import { frameVerdict } from './frames.mjs';

test('screen preflight wakes only a dark display, restores it and rejects a missed power event', async () => {
  let dark = true, toggles = 0;
  const read = async () => ({ screen_dark: dark });
  const sim = async (event) => { assert.equal(event, 'pwr'); toggles++; dark = !dark; };
  await setDeviceScreen(read, sim, false);
  await setDeviceScreen(read, sim, false);
  assert.equal(toggles, 1);
  await setDeviceScreen(read, sim, true);
  assert.equal(dark, true);
  await assert.rejects(setDeviceScreen(read, async () => {}, false, { timeoutMs: 5, intervalMs: 1 }), /did not become awake/);
});

test('device preflight waits for confirmed reconnect and rejects a persistently offline or broken bridge', async () => {
  let calls = 0;
  const read = async () => ({ok: true, online: ++calls >= 3, via: calls >= 3 ? 'wifi' : 'none'});
  const state = await waitForDeviceLink(read, {timeoutMs: 100, intervalMs: 1});
  assert.equal(calls, 3); assert.equal(state.online, true);
  await assert.rejects(waitForDeviceLink(async () => ({ok: true, online: false, via: 'none'}),
    {timeoutMs: 5, intervalMs: 1}), /authenticated connection/);
  await assert.rejects(waitForDeviceLink(async () => { throw Error('USB unavailable'); }), /USB unavailable/);
});

const snapshot = {ok:true, frames:30, elapsed_us:1000000, mean_us:20000, max_us:31000,
  late:0, face:3000, send:17000, gap_us:35000, character:'Tess', sequence:7, modes:16, active:1,
  presented:30,overflows:0};

test('stats require a complete framed snapshot even when logs are absent or stale', async () => {
  const reply = snapshot;
  const sim = async () => reply;
  assert.equal((await flushStats({sim})).frames, 30);
  assert.equal((await flushStats({sim})).modes, 16);
  for (const bad of [{}, {...reply, mean_us: undefined}, {...reply, sequence: undefined}, {...reply, modes: undefined},
    {...reply,presented:undefined},{...reply,overflows:undefined}])
    await assert.rejects(flushStats({sim: async () => bad}), MissingStats);
});

test('one missing observation is retried; numeric and other failures are never retried', async () => {
  let calls = 0;
  const result = await measureWindow(async () => { if (++calls === 1) throw new MissingStats('lost'); return { late: 99 }; }, () => {});
  assert.equal(calls, 2); assert.equal(result.late, 99);
  calls = 0;
  await assert.rejects(measureWindow(async () => { calls++; throw new MissingStats('lost'); }, () => {}), MissingStats);
  assert.equal(calls, 2);
  calls = 0;
  await assert.rejects(measureWindow(async () => { calls++; throw Error('device rejected'); }), /device rejected/);
  assert.equal(calls, 1);
});

test('frame decisions preserve raw FPS on both sides of the 29.5 boundary', async () => {
  for (const [elapsed_us, expected] of [[2035600, false], [2033800, true]]) {
    const sample = await flushStats({sim: async () => ({...snapshot, frames:60, presented:60, elapsed_us})});
    assert.equal(sample.fps, 60e6 / elapsed_us);
    assert.equal(frameVerdict(sample, 'conversation'), expected);
  }
});

const playing = {frames:66, presented:66, overflows:0, modes:16, active:4, fps:30,
  late:0, meanMs:25, maxMs:31, gapMs:36};
const finished = {active:-1, sequence:2, gen:4, reason:'played', ms:PROFILE_MS,
  gaps:0, gap_ms:0, rx_rejected:0, write_errors:0, late_feeds:0, heap_failures:0};
const faults = async () => ({ok:true,instance:'test',faults:0,boots:0,disconnects:0});

test('supplemental faults reject handled failures, reboot, collector replacement and USB detach', async () => {
  const baseline=await faults(); assertFaultWindow(baseline,baseline);
  for(const patch of [{faults:1},{boots:1},{disconnects:1},{instance:'restarted'},{ok:false},{faults:undefined}])
    assert.throws(()=>assertFaultWindow({...baseline,...patch},baseline));
  for (const patch of [{instance:undefined},{instance:''},{faults:-1},{boots:undefined},{disconnects:undefined}]) {
    const missing = {...baseline,...patch};
    assert.throws(() => assertFaultWindow(missing,missing));
  }
  assert.throws(()=>speechProfile({}), /supplemental USB fault collector/);
});

test('speech windows reject end, wrong generation, unpresented/mixed frames and the original numeric failures', () => {
  assertPlaybackWindow(playing, 4);
  for (const patch of [{active:-1}, {active:5}, {modes:4}, {modes:20}, {presented:65}, {overflows:1},
    {fps:29.4}, {frames:59,presented:59}, {late:6}, {meanMs:34}, {maxMs:41.1}, {gapMs:56.1}])
    assert.throws(() => assertPlaybackWindow({...playing,...patch},4));
});

test('retained completion accepts exact playback and rejects stale/cancelled/short/error counters', () => {
  const previous = {sequence:1,heap_failures:0};
  assertPlaybackCompletion(finished, previous);
  for (const patch of [{sequence:1},{active:4},{gen:undefined},{reason:'cancelled'},{ms:PROFILE_MS-1},
    {gaps:1},{gap_ms:1},{rx_rejected:1},{write_errors:1},{late_feeds:1},{heap_failures:1}])
    assert.throws(() => assertPlaybackCompletion({...finished,...patch},previous));
  for (const patch of [{heap_failures:undefined},{heap_failures:-1},{sequence:undefined},{sequence:1.5}])
    assert.throws(() => assertPlaybackCompletion({...finished,...patch},{...previous,...patch}));
});

test('profile produces all 8 seconds and succeeds from structured completion without any log file', async () => {
  let done = false, bytes = 0;
  const profile = speechProfile({burst:true, faults, measureFrames:false,
    diagnostics:async () => done ? finished : {sequence:1,heap_failures:0}});
  const played = await profile({onAudio(pcm){bytes+=pcm.length;}, onResponseDone(){done=true;}},()=>false,'live');
  assert.equal(bytes, PROFILE_MS*48);
  assert.equal(played.gen,4);
});

test('missing speaking coverage still validates exact completion before reporting the frame error', async () => {
  for (const milliseconds of [PROFILE_MS,PROFILE_MS-1]) {
    let done = false;
    const profile = speechProfile({burst:true,faults,sim:async()=>({...snapshot,active:-1}),
      diagnostics:async()=>done ? {...finished,ms:milliseconds} : {sequence:1,heap_failures:0}});
    await assert.rejects(profile({onAudio(){},onResponseDone(){done=true;}},()=>false,'live'),error =>
      milliseconds===PROFILE_MS ? /Speech never reached the screen/.test(error.message)
        : error.actual===PROFILE_MS-1 && error.expected===PROFILE_MS);
  }
});

import { assertIdleCapture } from './diagnostics.mjs';
test('idle capture distinguishes passive Tess from blocked states and Plush', () => {
  const off = {mic_open:false,auto_recording:false,mic_enabled:false,mic_rx_enabled:false,mic_idle_reads:0};
  assertIdleCapture({...off, character:'Plush'});
  const tess = {...off,character:'Tess',wake_listening:true,online:true,stt_known:true,stt_available:true,menu:false,screen_dark:false,mic_enabled:true,mic_rx_enabled:true};
  assertIdleCapture(tess);
  for (const patch of [{character:'Plush'}, {menu:true}, {screen_dark:true}, {online:false}, {stt_available:false}, {mic_open:true}, {mic_rx_enabled:false}])
    assert.throws(() => assertIdleCapture({...tess,...patch}));
  assert.throws(() => assertIdleCapture({...off,mic_enabled:true}));
});
test('idle preflight waits for ADC startup/teardown, rejects stuck hardware and never retries recording or idle reads', async () => {
  const off={mic_open:false,auto_recording:false,mic_idle_reads:0,mic_enabled:false,mic_rx_enabled:false};
  const on={...off,character:'Tess',wake_listening:true,online:true,stt_known:true,stt_available:true,
    menu:false,screen_dark:false,mic_enabled:true,mic_rx_enabled:true};
  for (const states of [[{...on,mic_rx_enabled:false},on],[{...off,mic_rx_enabled:true},off]]) {
    let calls=0;
    await waitForIdleCapture(async()=>states[Math.min(calls++,1)],{timeoutMs:100,intervalMs:1});
    assert.equal(calls,2);
  }
  await assert.rejects(waitForIdleCapture(async()=>({...on,mic_rx_enabled:false}),{timeoutMs:5,intervalMs:1}),/idle capture hardware/);
  for(const patch of [{mic_open:true},{auto_recording:true},{mic_idle_reads:1}]) {
    let calls=0;
    await assert.rejects(waitForIdleCapture(async()=>{calls++;return {...on,...patch};}));
    assert.equal(calls,1);
  }
});
