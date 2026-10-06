#!/usr/bin/env node
// Physical native input/output boundaries with the production session and a fake SDK provider.
// --wifi profiles 8 s of burst PCM over real TLS; --paced tests incremental delivery.
// --single-delta tests one 8 s provider callback; --small-deltas tests 20 ms callbacks.
// Point the USB bridge at ws://127.0.0.1:18999/kubik/v1. No paid calls or wake-phrase audio.
import assert from 'node:assert/strict';
import { EchoCleaner } from '../openclaw-kubik/src/echo-cleaner.js';
import { nearSpeechProof } from './test-device/live-acoustic.mjs';
import { writeFileSync } from 'node:fs';
import { nativeTransport } from './test-device/native-transport.mjs';
import { waitForDeviceLink } from './test-device/diagnostics.mjs';
import { speechProfile, PROFILE_MS } from './test-device/speech-profile.mjs';
import { setTimeout as sleep } from 'node:timers/promises';
import { KubikServer } from '../openclaw-kubik/src/server.js';
import { LiveVoiceSession } from '../openclaw-kubik/src/live-voice.js';
import { NativeVoiceTurn } from '../openclaw-kubik/src/native-voice.js';
import { resolveAccount } from '../openclaw-kubik/src/config.js';
import { decodeDeviceKey } from '../openclaw-kubik/src/protocol.js';
import { fakeEngine, fakePairing, tone } from '../openclaw-kubik/test/helpers.js';

const controlUrl = process.env.KUBIK_CONTROL_URL ?? 'http://127.0.0.1:18791';
async function controlRequest(body) {
  const response = await fetch(`${controlUrl}/config`, { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body),
    signal: AbortSignal.timeout(5000) });
  if (response.status === 504) throw Object.assign(Error(`Device did not reply to ${body.cmd}`), { status: 504 });
  assert.equal(response.status, 200);
  const state = await response.json(); assert.equal(state.ok, true); return state;
}
let controlQueue = Promise.resolve(), controlAt = 0;
const control = body => {
  const pending = controlQueue.then(async () => {
    await sleep(Math.max(0, controlAt + 150 - Date.now()));
    controlAt = Date.now(); return controlRequest(body);
  });
  controlQueue = pending.catch(() => {}); return pending;
};
const wifi = process.argv.includes('--wifi');
const acousticOnly = process.argv.includes('--acoustic-only');
if (acousticOnly) assert.ok(process.argv.includes('--near') && process.argv.includes('--echo'), '--acoustic-only requires --near --echo');
const profile = wifi || process.argv.includes('--profile') || acousticOnly;
const speechOnly = process.argv.includes('--speech-only') || acousticOnly;
const info = () => control({ cmd: 'info' });
const sim = (ev, extra = {}) => control({ cmd: 'sim', ev, ...extra });
async function hostUntil(check, label, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  do { if (check()) return; await sleep(100); } while (Date.now() < deadline);
  throw Error(`Timed out: ${label}`);
}
async function until(check, label, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  let state;
  do {
    try { state = await info(); }
    catch (error) {
      if (error.status !== 504) throw error;
      // A settings change reboots the device. Only repeat the read, never a write.
      await sleep(100); continue;
    }
    if (await check(state)) return state;
    await sleep(100);
  } while (Date.now() < deadline);
  const safe = Object.fromEntries(['online', 'via', 'voice_mode', 'mic_open', 'mic_enabled', 'wake_listening'].map(key => [key, state?.[key]]));
  throw Error(`Timed out: ${label}; ${JSON.stringify(safe)}`);
}

function nativeEngine(state) {
  const engine = { ...fakeEngine(), canListen: true, canSpeak: false,
    get voiceMode() { return state.mode; }, nativeVoice: { available: true } };
  engine.createVoiceTurn = (options) => {
    if (state.failBeforeCapture) {
      state.startupFailures = (state.startupFailures ?? 0) + 1;
      throw Error('Mock SDK startup failure');
    }
    let fired = false, closed = false, replies = 0, silence, silenceStop;
    const mode = state.mode;
    if (mode === 'live') state.liveConnections = (state.liveConnections ?? 0) + 1;
    const emit = callbacks => {
      if (!state.speech) {
        callbacks.onAudio(tone(500));
        if (mode === 'live' && !silence) {
          // Continuous Live transport also sends digital silence while played is pending.
          silence = setInterval(() => { if (!closed) callbacks.onAudio(Buffer.alloc(960)); }, 20);
          silenceStop = setTimeout(() => clearInterval(silence), 8000);
        }
        if (mode === 'realtime') callbacks.onResponseDone({ status: 'completed' });
        return;
      }
      state.profile = state.speech(callbacks, () => closed, mode).catch(error => {
        state.profileError = error; callbacks.onError(error);
      });
    };
    const provider = { createBridge(callbacks) {
      if (mode === 'live') {
        state.liveCallbacks = callbacks;
        state.stopTransportSilence = () => { clearInterval(silence); clearTimeout(silenceStop); };
      }
      return { connect: async () => {}, pacesInputAudio: mode === 'live',
        isConnected: () => !closed, acknowledgeMark() {}, setMediaTimestamp() {}, submitToolResult() {},
        close: () => { closed = true; clearInterval(silence); clearTimeout(silenceStop); },
        sendAudio() {
          state.audioFrames++;
          if (fired || state.hold || (mode === 'live' && replies && !state.nextLiveReply) || closed || replies >= (mode === 'live' ? 2 : 1)) return;
          fired = true; replies++; state.nextLiveReply = false;
          setTimeout(async () => {
            if (closed) return;
            callbacks.onTranscript('user', 'Physical native session test', true);
            if (mode === 'live') {
              await callbacks.runAgentConsult({ prompt: 'Physical native session test' });
              if (!closed) { callbacks.onTranscript('assistant', 'Mock answer', false); emit(callbacks); fired = false; }
            }
          }, 100);
        },
        sendUserMessage() {
          if (closed) return;
          emit(callbacks);
        } };
    } };
    const Voice = mode === 'live' ? LiveVoiceSession : NativeVoiceTurn;
    const turn = new Voice({ ...options,
      config: { mode, provider, providerConfig: {}, capabilities: {}, cfg: {} }, quietMs: profile ? 3000 : 500 });
    if (mode === 'live') {
      const append = turn.append.bind(turn);
      turn.append = (pcm, reference) => {
        if (state.samples && state.samples.length < 750) state.samples.push({ mic: Buffer.from(pcm), reference: Buffer.from(reference) });
        append(pcm, reference);
      };
    }
    state.lastTurn = turn; state.lastTurnId = options.turnId;
    return turn;
  };
  return engine;
}

async function setMode(server, state, mode) {
  const session = server.getSession(state.device);
  assert.ok(session && !session.closed);
  state.mode = mode;
  assert.equal(session.send({ t: 'capabilities', voice_mode: mode, stt: { available: true }, tts: { available: true } }), true);
  console.log(`SEND ${mode} mode on session ${session.sessionId}, pending=${session.ws.bufferedAmount}`);
  await until(device => device.voice_mode === mode && !device.mic_open, `${mode} capability applied`);
  if (mode === 'live' && (await info()).character === 'Tess')
    await until(device => device.wake_listening, 'Live passive activation ready');
  console.log(`PASS physical ${mode} capability and idle activation policy`);
}

function echoProof(state) {
      const cleaner = new EchoCleaner(), clean = state.samples.map(frame => cleaner.process(frame.mic, frame.reference));
      cleaner.close();
      const frames = state.samples.filter((frame, i) => i >= 75 && i < state.samples.length - 25);
      let near = 0, residual = 0, far = 0, clipped = 0, samples = 0;
      for (let i = 75; i < state.samples.length - 25; i++) for (let at = 0; at < 1920; at += 2) {
        const mic = state.samples[i].mic.readInt16LE(at), ref = state.samples[i].reference.readInt16LE(at);
        near += mic * mic; residual += clean[i].readInt16LE(at) ** 2; far += ref * ref;
        clipped += Math.abs(ref) >= 32000; samples++;
      }
      writeFileSync('/tmp/kubik-live-mic.pcm', Buffer.concat(state.samples.map(frame => frame.mic)));
      writeFileSync('/tmp/kubik-live-reference.pcm', Buffer.concat(state.samples.map(frame => frame.reference)));
      writeFileSync('/tmp/kubik-live-clean.pcm', Buffer.concat(clean));
      const erle = 10 * Math.log10(near / residual);
      console.log(`ECHO physical: frames=${frames.length} refRMS=${Math.sqrt(far / samples).toFixed(0)} refClipped=${clipped}/${samples} ERLE=${erle.toFixed(1)}dB`);
      assert.ok(Math.sqrt(far / samples) > 100, 'Codec speaker reference is absent');
      assert.ok(clipped / samples < .001, 'Codec speaker reference is clipped');
      assert.ok(erle > 10, 'Physical far-end echo cancellation did not reduce echo enough');
}

async function bargeProof(state) {
  state.stopTransportSilence(); // Exact replacement length excludes the preceding silence fixture.
  const receipts = state.receipts.length, cancellations = state.cancellations.length;
  const previousGen = state.progress?.gen;
  state.liveCallbacks.onAudio(tone(3000));
  await hostUntil(() => state.progress?.gen !== previousGen && state.progress?.ms > 0, 'Live plays before interruption');
  assert.equal((await info()).mic_open, true);
  const input = state.audioFrames;
  state.liveCallbacks.onClearAudio('barge-in');
  const replacementAt = Date.now();
  state.liveCallbacks.onAudio(tone(500));
  await hostUntil(() => state.cancellations.length > cancellations, 'physical output cancellation ACK');
  const cancelled = state.cancellations.at(-1);
  await hostUntil(() => (state.progress?.gen !== cancelled.gen && state.progress?.ms > 0) ||
    (state.receipts.length > receipts && state.receipts.at(-1).gen !== cancelled.gen), 'short replacement reaches hardware within two seconds', 2000);
  const startupMs = Date.now() - replacementAt;
  assert.ok(startupMs < 2000, 'Short reply waited for the provider quiet boundary');
  console.log(`PASS short Live reply reached hardware within ${startupMs}ms`);
  await hostUntil(() => state.receipts.length > receipts, 'replacement reply played');
  assert.equal(state.receipts.at(-1).ms, 500, 'Replacement included discarded speaker audio');
  assert.notEqual(state.receipts.at(-1).gen, cancelled.gen, 'Cancelled generation finished as replacement');
  assert.ok(state.audioFrames > input, 'Output cancellation stopped continuous input');
  assert.equal(state.liveConnections, 1, 'Output cancellation reopened the Live provider');
  await until(device => device.live_active && device.live_ready && device.mic_open, 'Live stays open after interruption');
  console.log('PASS physical Live barge-in: exact-generation flush, new reply and continuous input');
}

async function acousticProof(state) {
  if (state.samples) echoProof(state);
  if (process.argv.includes('--near')) await nearSpeechProof(state, { info, hostUntil });
}

async function responseProof(server, state, mode) {
  await setMode(server, state, mode);
  if (mode === "live" && process.argv.includes("--echo")) state.samples = [];
  const before = await info();
  const receipts = state.receipts.length;
  await sim('ptt_down');
  await until(device => device.mic_open && device.mic_rx_enabled, `${mode} KEY opens microphone`);
  if (mode === 'realtime') await until(device => !device.mic_open && !device.mic_enabled, 'Realtime closes capture before playback');
  else {
    await until(device => device.live_ready && device.mic_open, 'Live continuous capture ready');
    const input = state.audioFrames;
    await sleep(400);
    assert.ok(state.audioFrames >= input + 5, 'Live input stopped during output');
    assert.equal((await info()).mic_open, true, 'Live playback closed the microphone');
  }
  assert.ok(state.audioFrames > 0, 'physical PCM never reached the native bridge');
  await hostUntil(() => { if (state.profileError) throw state.profileError; return state.receipts.length > receipts; }, `${mode} physical playback ACK`);
  const playedMs = state.receipts.at(-1).ms;
  if (mode === 'live' && !profile) {
    // The first stream keeps quietMs of trailing transport silence before played.
    assert.ok(playedMs >= 500 && playedMs <= 1100, 'Live lost tone or retained unbounded silence');
  } else assert.equal(playedMs, profile ? PROFILE_MS : 500, 'speaker acknowledged insufficient PCM');
  await state.profile;
  assert.ok((await info()).mic_reads > before.mic_reads);
  await sim('ptt_up');
  if (mode === 'live') {
    await until(device => device.live_active && device.live_ready && device.mic_open, 'Live resumes after first reply');
    state.nextLiveReply = true;
    await hostUntil(() => { if (state.profileError) throw state.profileError; return state.receipts.length >= receipts + 2; }, 'second reply on the same Live connection');
    await until(device => device.live_active && device.live_ready && device.mic_open, 'Live resumes after second reply');
    assert.equal(state.liveConnections, 1, 'Live reopened SDK between questions');
    await state.profile;
    if (state.profileError) throw state.profileError;
    if (!profile) {
      await sleep(1000);
      assert.equal(state.receipts.length, receipts + 2, 'transport silence created extra speaker generations');
      assert.equal((await info()).mic_open, true, 'transport silence closed input again');
    }
    await acousticProof(state);
    if (process.argv.includes('--barge')) await bargeProof(state);
    await sim('ptt_down'); await sim('ptt_up');
    await until(device => !device.live_active && !device.mic_open, 'KEY explicitly ends Live');
  }
  await state.profile; if (state.profileError) throw state.profileError;
  console.log(`PASS physical ${mode}: native input policy, same agent and speaker ACK${mode === 'live' ? ', two replies on one connection, continuous silence and KEY stop' : ''}`);
}

async function passiveWakeProof(server, state, character) {
  await setMode(server, state, 'live');
  await setMode(server, state, 'realtime');
  if (character === 'Plush') {
    assert.equal((await info()).wake_listening, false);
    return;
  }
  const awake = () => until(device => device.wake_listening && device.mic_enabled, 'Realtime passive listener ready');
  const asleep = () => until(device => !device.wake_listening && !device.mic_enabled, 'passive listener suspended');
  await awake();
  await sim('menu'); await asleep();
  await sim('menu'); await awake();
  await sim('pwr'); await asleep();
  await sim('pwr'); await awake();
  console.log('PASS physical Realtime passive listener resumes after Live, menu and screen sleep; no wake phrase played');
}

async function inputContrasts(server, state) {
  state.hold = true;
  await setMode(server, state, 'realtime');
  const oldTurn = state.lastTurnId;
  await sim('ptt_down');
  await until(device => device.mic_open, 'new Realtime capture');
  const session = server.getSession(state.device);
  assert.notEqual(state.lastTurnId, oldTurn);
  session.send({ t: 'input_end', turn: oldTurn, session: session.sessionId });
  await sleep(300);
  assert.equal((await info()).mic_open, true, 'old turn stopped the new microphone');
  state.lastTurn.endInput();
  await until(device => !device.mic_open, 'current turn closes microphone');
  state.lastTurn.fail(); // Finish the held mock turn without starting another KEY capture.
  await sim('ptt_up');
  await sleep(300);
  console.log('PASS physical input_end rejects an old turn and accepts the current turn');

  await setMode(server, state, 'live');
  const failures = state.startupFailures ?? 0, receipts = state.receipts.length;
  const dispatches = state.dispatches, audioFrames = state.audioFrames;
  state.failBeforeCapture = true;
  try {
    await sim('ptt_down');
    // KEY capture and remote SDK startup overlap. A delayed input_end can arrive
    // after ADC startup. Require sustained closure, not a pre-start snapshot.
    let stableAt = Date.now();
    await until(device => {
      const closed = state.startupFailures > failures && !device.mic_open && !device.live_active;
      if (!closed) stableAt = Date.now();
      return closed && Date.now() - stableAt >= 400;
    }, 'SDK failure closes Live capture', 5000);
    assert.equal(state.receipts.length, receipts, 'SDK failure produced speech');
    assert.equal(state.dispatches, dispatches, 'SDK failure dispatched to the agent');
    assert.equal(state.audioFrames, audioFrames, 'SDK failure sent provider audio');
    await sim('ptt_up');
  } finally { state.failBeforeCapture = false; }
  console.log('PASS physical SDK startup failure closes ADC/RX without a response');
  await sim('ptt_down');
  await until(device => device.mic_open, 'Live capture without a transcript');
  await sim('ptt_up');
  await sleep(9000);
  assert.equal((await info()).live_active, true, 'Live expired at a silence timeout');
  assert.equal((await info()).mic_open, true, 'Live stopped capture without an explicit end');
  // Local mock only: no room audio is sent to a paid provider.
  state.lastTurn.endInput();
  await until(device => !device.mic_open && !device.live_active, 'Live input_end releases capture');
  state.lastTurn.fail();
  await sim('ptt_up');
  console.log('PASS physical Live survives silence and release; explicit input_end closes it');
  await sim('ptt_down'); await sim('ptt_up');
  await until(device => device.live_active && device.mic_open, 'Live before menu');
  await sim('menu');
  await until(device => device.menu && !device.live_active && !device.mic_open, 'menu closes Live');
  await sim('menu');
  await sim('ptt_down'); await sim('ptt_up');
  await until(device => device.live_active && device.mic_open, 'Live before screen off');
  await sim('pwr');
  await until(device => device.screen_dark && !device.live_active && !device.mic_open, 'screen off closes Live');
  await sim('pwr');
  console.log('PASS physical menu and screen off explicitly close Live');


  await setMode(server, state, 'classic');
  await sim('ptt_down'); await sleep(1200);
  assert.equal((await info()).mic_open, true, 'classic manual capture ended at a pause');
  await sim('ptt_up'); await until(device => !device.mic_open, 'classic release');
  console.log('PASS physical classic manual capture retains KEY control during silence');
}

async function nativeProofs(server, state, character) {
  if (!speechOnly) await passiveWakeProof(server, state, character);
  const liveOnly = process.argv.includes('--live-only') || acousticOnly;
  if (!liveOnly) await responseProof(server, state, 'realtime');
  await responseProof(server, state, 'live');
  assert.equal(state.dispatches, liveOnly ? 2 : 3, 'Every native question must delegate to the agent');
  if (!speechOnly) await inputContrasts(server, state);
}

async function main() {
  const original = await waitForDeviceLink(info);
  console.log('BASELINE', Object.fromEntries(['online', 'via', 'voice_mode', 'wifi_connected'].map(key => [key, original[key]])));
  assert.ok(['Tess', 'Plush'].includes(original.character));
  assert.ok(original.voice_mode, 'Flash the native voice firmware before this check');
  const state = { device: original.device, mode: 'realtime', audioFrames: 0, dispatches: 0, hold: false, receipts: [], cancellations: [] };
  const store = fakePairing({ allowed: [`${original.device}:${decodeDeviceKey(original.key).fingerprint}`] });
  const pairing = { allowed: () => store.api.readAllowFromStore({ channel: 'kubik', accountId: 'default' }),
    upsert: () => { throw Error('Unexpected unapproved physical device'); } };
  const account = resolveAccount({ channels: { kubik: { voice: { mode: 'realtime' } } } });
  const server = new KubikServer({ account, pairing, log: message => console.log(`HOST ${message}`), engineFactory: () => nativeEngine(state),
    dispatch: async ({ deliver, device }) => {
      assert.equal(device.id, original.device); state.dispatches++; await deliver('Mock agent answer.');
    } });
  server.start();
  let transport;
  let paused = false;
  try {
    transport = await nativeTransport(server, original, wifi);
    if (profile) state.speech = speechProfile({ sim, info, measureFrames: !acousticOnly, burst: !process.argv.includes('--paced'),
      chunkMs: process.argv.includes('--single-delta') ? PROFILE_MS : process.argv.includes('--small-deltas') ? 20 : 100 });
    paused = true; await transport.enter(sim, control);
    await until(device => device.online && device.via === transport.via && server.getSession(original.device), 'authenticated production transport', 40000);
    server.getSession(original.device).ws.on('message', (data, binary) => {
      if (!binary) {
        const message = JSON.parse(String(data));
        if (message.t === 'played') state.receipts.push(message);
        if (message.t === 'cancelled') state.cancellations.push(message);
        if (message.t === 'progress') state.progress = message;
      }
    });
    if ((await info()).screen_dark) await sim('pwr');
    if ((await info()).menu) await sim('menu');
    await nativeProofs(server, state, original.character);
  } finally {
    try {
      // Release the mock stream/TLS before writing settings, including after heap exhaustion.
      await server.stop();
      await until(device => Number.isInteger(device.heap) && typeof device.url === 'string', 'complete status before restoration', 45000);
      try { if ((await info()).mic_open) { await sim('ptt_up'); await sim('ptt_down'); await sim('ptt_up'); } }
      finally { if (paused) await transport.restore(sim, control); }
    } finally { await transport?.close(); }
    const restored = await until(device => device.via === original.via && device.wifi_connected === original.wifi_connected && device.voice_mode === original.voice_mode && Boolean(device.key),
      'original network route', 45000);
    for (const key of ['volume', 'brightness', 'ssid', 'url', 'key', 'character', 'server_pinned', 'guide_done'])
      assert.ok(restored[key] === original[key], `${key} changed`);
    if (restored.screen_dark !== original.screen_dark) await sim('pwr');
    if (restored.menu !== original.menu) await sim('menu');
    console.log('PASS original network, pairing identity, volume, brightness and screen restored');
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
