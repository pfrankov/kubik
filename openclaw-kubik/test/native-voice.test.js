import assert from 'node:assert/strict';
import test from 'node:test';
import { NativeVoiceTurn, resolveNativeVoice } from '../src/native-voice.js';
import { LiveVoiceSession } from '../src/live-voice.js';
import { DeviceSession } from '../src/session.js';
import { SPEECH_LEAD_MS, MAX_NATIVE_AUDIO_BYTES } from '../src/protocol.js';
import { resolveAccount } from '../src/config.js';
import { probeAccount } from '../src/channel.js';
import { acknowledgeNativeInput, fakeEngine, sleep } from './helpers.js';

function fixture(t, { mode = 'realtime', dispatch, quietMs = 20, connect } = {}) {
  let callbacks;
  const calls = { audio: [], messages: [], close: [], dispatch: [], end: 0, done: 0, fail: 0, replies: 0, pauses: 0, resumes: 0, text: [] };
  const bridge = { connect: connect ?? (async () => {}), sendAudio: (pcm) => calls.audio.push(pcm),
    sendUserMessage: (text) => calls.messages.push(text), close: (options) => { calls.close.push(options); },
    pacesInputAudio: mode === 'live', isConnected: () => true, acknowledgeMark() {}, setMediaTimestamp() {},
    submitToolResult() {} };
  const provider = { createBridge(options) { callbacks = options; return bridge; } };
  let current = true;
  const Voice = mode === 'live' ? LiveVoiceSession : NativeVoiceTurn;
  const turn = new Voice({ config: { mode, provider, providerConfig: {}, capabilities: {}, cfg: {}, language: 'ru' },
    turnId: 7, quietMs, dispatch: dispatch ?? (async (request) => {
      calls.dispatch.push(request); await request.deliver('The existing agent answer.');
    }), port: { isCurrent: () => current, endInput: () => calls.end++, finish: () => calls.done++,
      pauseInput: async () => { calls.pauses++; }, resumeInput: async () => { calls.resumes++; },
      finishReply: async () => { calls.replies++; },
      fail: () => calls.fail++, text: (text) => calls.text.push(text),
      reply: (answer, shown) => { calls.text.push(answer); if (shown) calls.text.push(shown); },
      audio: (pcm) => { calls.output = pcm; return true; } } });
  t.after(() => { current = false; turn.close(); });
  return { turn, callbacks, bridge, calls };
}

test('native metadata uses the SDK gateway-relay contract without connecting a provider', async () => {
  const seen = [];
  const provider = { resolveConfig: (request) => { seen.push(request); return request.rawConfig; },
    isConfigured: (request) => { seen.push(request); return true; }, createBridge() { throw Error('Must not connect'); } };
  const result = await resolveNativeVoice({ cfg: {}, agentId: 'device-agent', voice: { mode: 'live', voice: 'marin' },
    sdk: { getRealtimeVoiceProvider: () => provider, resolveRealtimeVoiceProviderCapabilities: (request) => {
      assert.equal(request.provider, provider); assert.equal(request.surface, 'gateway-relay'); return {};
    } } });
  assert.equal(result.providerConfig.model, 'gpt-live-1'); assert.equal(result.providerConfig.silenceDurationMs, 800); assert.equal(result.stt.available, true);
  assert.ok(seen.every(request => request.agentId === 'device-agent'));
});

test('removed native provider reports unavailability without losing the device settings connection', async () => {
  const result = await resolveNativeVoice({ cfg: {}, voice: { mode: 'live' },
    sdk: { getRealtimeVoiceProvider: () => undefined } });
  assert.equal(result.mode, 'live'); assert.equal(result.available, false);
  assert.equal(result.stt.available, false); assert.equal(result.tts.available, false);
});

test('selection rejects native HTTP and duplicate live-STT paths; classic remains default', () => {
  const account = (voice) => resolveAccount({ channels: { kubik: { voice } } });
  assert.equal(account({}).voice.mode, 'classic');
  assert.equal(account({ mode: 'realtime' }).voice.mode, 'realtime');
  assert.throws(() => account({ mode: 'live', provider: 'openai-http' }), /require openclaw/);
  assert.throws(() => account({ mode: 'realtime', liveTranscription: true }), /liveTranscription disabled/);
  assert.throws(() => account({ mode: 'invalid' }), /mode is not supported/);
});

test('channel probe reports selected native readiness independently of classic STT and TTS', async () => {
  const cfg = { channels: { kubik: { voice: { mode: 'realtime' } } } };
  const account = resolveAccount(cfg);
  for (const available of [true, false]) {
    const probe = await probeAccount({ account, cfg }, async ({ voice }) => {
      assert.equal(voice.mode, 'realtime');
      return { stt: { available }, tts: { available } };
    });
    assert.equal(probe.ok, available); assert.equal(probe.stt.available, available);
    assert.equal(probe.tts.available, available);
    if (!available) { assert.match(probe.error, /Native voice/); assert.match(probe.warning, /Native voice/); }
  }
});

test('Realtime final input closes capture then delegates once through the existing dispatcher', async (t) => {
  const { turn, callbacks, calls } = fixture(t);
  assert.match(callbacks.instructions, /Speak in ru\./);
  turn.append(Buffer.alloc(1920)); await sleep(0);
  assert.equal(calls.audio.length, 1);
  callbacks.onTranscript('user', 'My question', false); assert.equal(calls.dispatch.length, 0);
  callbacks.onEvent({ type: 'input_audio_buffer.speech_stopped' });
  callbacks.onTranscript('user', 'My question', true); await sleep(0);
  assert.equal(calls.end, 1); assert.equal(calls.dispatch[0].transcript, 'My question');
  assert.equal(calls.dispatch[0].turnId, 7); assert.equal(calls.dispatch[0].isCurrent(), true);
  assert.match(calls.messages[0], /The existing agent answer/);
  const count = calls.audio.length; turn.append(Buffer.alloc(1920)); assert.equal(calls.audio.length, count);
  callbacks.onAudio(Buffer.alloc(1920, 20)); callbacks.onResponseDone({ status: 'completed' });
  await sleep(0);
  assert.equal(calls.done, 1); assert.deepEqual(calls.close, [{ disposition: 'detach' }]);
});

test('GPT-Live filler cannot finish accepted agent work; quiet completes the reply and retains the session', async (t) => {
  let release;
  const pending = new Promise(resolve => { release = resolve; });
  const { callbacks, calls } = fixture(t, { mode: 'live', dispatch: async ({ deliver }) => {
    await pending; await deliver('Agent tool completed.');
  } });
  await sleep(0);
  const consult = callbacks.runAgentConsult({ prompt: 'Use my tools' });
  callbacks.onAudio(Buffer.alloc(1920, 20)); callbacks.onTranscript('assistant', 'One moment', true);
  await sleep(35); assert.equal(calls.done, 0);
  release(); assert.equal((await consult).text, 'Agent tool completed.');
  callbacks.onAudio(Buffer.alloc(1920, 20)); callbacks.onTranscript('assistant', 'Done.', false);
  await sleep(35); assert.equal(calls.done, 0); assert.equal(calls.end, 0);
  assert.equal(calls.replies, 1); assert.equal(calls.resumes, 1); assert.equal(calls.close.length, 0);
});

test('disconnect detaches voice without aborting already accepted tools and drops late output', async (t) => {
  let release, accepted;
  const pending = new Promise(resolve => { release = resolve; });
  const { turn, callbacks, calls } = fixture(t, { mode: 'live', dispatch: async (request) => {
    accepted = request; await pending; await request.deliver('Tool result');
  } });
  await sleep(0);
  const consult = callbacks.runAgentConsult({ prompt: 'Action' }); await sleep(0);
  assert.ok(accepted, 'Agent accepts the tool only after the microphone pause');
  turn.close(); assert.equal(accepted.isCurrent(), false); assert.equal(accepted.abortSignal, undefined);
  callbacks.onAudio(Buffer.alloc(1920, 20)); callbacks.onTranscript('assistant', 'Late', true);
  release(); await assert.rejects(consult, /superseded/);
  assert.equal(calls.output, undefined); assert.deepEqual(calls.text, []); assert.equal(calls.done, 0);
  assert.deepEqual(calls.close, [{ disposition: 'detach' }]);
});

test('connecting microphone queue is bounded and an oversized agent reply fails', async (t) => {
  const blocked = fixture(t, { connect: () => new Promise(() => {}) });
  blocked.turn.append(Buffer.alloc(240_002));
  assert.equal(blocked.calls.fail, 1); assert.equal(blocked.calls.end, 1);
  const oversized = fixture(t, { dispatch: ({ deliver }) => deliver('a'.repeat(8193)) });
  await sleep(0); oversized.callbacks.onTranscript('user', 'Question', true); await sleep(0);
  assert.equal(oversized.calls.fail, 1); assert.equal(oversized.calls.messages.length, 0);
});

test('native output accepts a multi-second provider delta and rejects malformed or over-budget PCM', async (t) => {
  for (const mode of ['realtime', 'live']) {
    const { turn, callbacks, calls } = fixture(t, { mode });
    await sleep(0);
    if (mode === 'live') callbacks.onTranscript('user', 'Question', true); else turn.endInput();
    const pcm = Buffer.alloc(3000 * 48, 20);
    callbacks.onAudio(pcm);
    assert.equal(calls.output, pcm); assert.equal(calls.fail, 0, mode);
    callbacks.onAudio(Buffer.alloc(MAX_NATIVE_AUDIO_BYTES + 2));
    assert.equal(calls.fail, 1); assert.equal(calls.output, pcm, 'invalid chunk was forwarded');
  }
  const malformed = fixture(t); await sleep(0); malformed.turn.endInput();
  malformed.callbacks.onAudio(Buffer.alloc(3));
  assert.equal(malformed.calls.fail, 1); assert.equal(malformed.calls.output, undefined);
});

test('native queue bounds accumulated deltas while device progress is stalled', async (t) => {
  let port;
  const engine = { ...fakeEngine(), voiceMode: 'realtime',
    createVoiceTurn(options) { port = options.port; return { close() {} }; } };
  const session = new DeviceSession({ device: { id: 'desk' }, engine, volume: 40,
    dispatch: async () => {}, ws: { readyState: 1, bufferedAmount: 0, send(value) { acknowledgeNativeInput(session, value); } } });
  t.after(() => session.close());
  session.handleMessage({ t: 'ptt', on: true, turn: 1 }); port.endInput();
  assert.equal(port.audio(Buffer.alloc(59_000 * 48)), true); await sleep(0);
  assert.equal(session.pacer.sentMs, SPEECH_LEAD_MS);
  assert.equal(port.audio(Buffer.alloc(3000 * 48)), false, 'accepted more than one minute of pending PCM');
  assert.ok(session.pacer.queuedBytes <= MAX_NATIVE_AUDIO_BYTES);
});

test('SDK input send failures end only the voice turn, including the silence timer', async (t) => {
  const direct = fixture(t); await sleep(0);
  direct.bridge.sendAudio = () => { throw Error('Mock transport send failure'); };
  assert.doesNotThrow(() => direct.turn.append(Buffer.alloc(1920)));
  assert.equal(direct.calls.fail, 1); assert.equal(direct.calls.end, 1);
  await sleep(0); assert.deepEqual(direct.calls.close, [{ disposition: 'detach' }]);
  const tail = fixture(t); await sleep(0);
  tail.bridge.sendAudio = () => { throw Error('Mock transport send failure'); };
  tail.turn.endInput(); await sleep(65);
  assert.equal(tail.calls.fail, 1); assert.equal(tail.calls.end, 1);
  assert.deepEqual(tail.calls.close, [{ disposition: 'detach' }]);
  await sleep(65); assert.equal(tail.calls.fail, 1, 'failed timer must not retry');
});

test('Live greeting and screen-only agent reply resume capture without closing the session', async (t) => {
  const greeting = fixture(t, { mode: 'live' }); await sleep(0);
  greeting.callbacks.onTranscript('assistant', 'Hello!', false);
  greeting.callbacks.onAudio(Buffer.alloc(1920, 20)); await sleep(35);
  assert.equal(greeting.calls.done, 0); assert.equal(greeting.calls.replies, 1); assert.equal(greeting.calls.dispatch.length, 0);
  const shown = fixture(t, { mode: 'live', dispatch: ({ deliver }) => deliver('[[show]]Details on screen[[/show]]') });
  await sleep(0); await shown.callbacks.runAgentConsult({ prompt: 'Show details' }); await sleep(0);
  assert.equal(shown.calls.done, 0); assert.equal(shown.calls.replies, 1); assert.equal(shown.calls.fail, 0);
});

test('native delegation keeps screen-only markup out of spoken results', async (t) => {
  const { callbacks, calls } = fixture(t, { mode: 'live', dispatch: ({ deliver }) =>
    deliver('[[happy]]Done. [[show]]https://example.org[[/show]] ```code```') });
  await sleep(0);
  const result = await callbacks.runAgentConsult({ prompt: 'Finish' });
  assert.equal(result.text, 'Done.'); assert.ok(calls.text.includes('https://example.org'));
});

test('Live joins every screen block and keeps its card when assistant transcripts follow', async (t) => {
  const { callbacks, calls } = fixture(t, { mode: 'live', dispatch: ({ deliver }) =>
    deliver('[[show]]First[[/show]] Spoken. [[show]]Second[[/show]]') });
  await sleep(0);
  const result = await callbacks.runAgentConsult({ prompt: 'Show both' });
  assert.equal(result.text, 'Spoken.'); assert.equal(calls.text.at(-1), 'First\nSecond');
  callbacks.onTranscript('assistant', 'Spoken.', false);
  assert.equal(calls.text.at(-1), 'First\nSecond');
});

test('Live forwards exact allowed session controls without the native XML envelope', async (t) => {
  const { callbacks, calls } = fixture(t, { mode: 'live' }); await sleep(0);
  callbacks.onTranscript('user', '/model openai/test', false);
  await callbacks.runAgentConsult({ prompt: '<realtime_delegation><input>/model openai/test</input></realtime_delegation>' });
  assert.equal(calls.dispatch[0].transcript, '/model openai/test');
});

test('Session native output shares generations/ACKs and queues notifications behind a recorded turn', async (t) => {
  let port;
  const events = [], binaries = [];
  const engine = { ...fakeEngine(), voiceMode: 'realtime', nativeVoice: { available: true },
    createVoiceTurn(options) { port = options.port; return { append() {}, endInput: port.endInput, close() {} }; } };
  const session = new DeviceSession({ device: { id: 'desk' }, engine, sessionId: 'scope', volume: 40,
    dispatch: async () => {}, ws: { readyState: 1, bufferedAmount: 0, send(value) {
      acknowledgeNativeInput(session, value);
      if (typeof value === 'string') events.push(JSON.parse(value)); else binaries.push(value);
    } } });
  t.after(() => session.close()); session.start();
  session.handleMessage({ t: 'ptt', on: true, turn: 4 });
  const notify = session.notify('Notification'); notify.catch(() => {}); await sleep(0);
  assert.equal(events.some(e => e.t === 'speak'), false);
  port.endInput(); port.audio(Buffer.alloc(4800, 20)); port.finish(); await sleep(10);
  const end = events.find(e => e.t === 'input_end'); assert.deepEqual(end, { t: 'input_end', turn: 4, session: 'scope' });
  assert.equal(events.find(e => e.t === 'speak').kind, 'reply'); assert.ok(binaries.length);
  assert.equal(events.filter(e => e.t === 'speak').length, 1);
  const gen = events.find(e => e.t === 'speak_end').gen;
  session.handleMessage({ t: 'played', gen, ms: 100 }); await sleep(10);
  assert.equal(events.filter(e => e.t === 'speak').length, 2);
});

test('GPT-Live accepts automatic activation once; muted native replies use text', async (t) => {
  let port, starts = 0;
  const events = [];
  const engine = { ...fakeEngine(), voiceMode: 'live', nativeVoice: { available: true },
    createVoiceTurn(options) { starts++; port = options.port; return { append() {}, endInput: port.endInput, close() {} }; } };
  const session = new DeviceSession({ device: { id: 'desk' }, engine, volume: 19, dispatch: async () => {},
    ws: { readyState: 1, bufferedAmount: 0, send(value) { if (typeof value === 'string') events.push(JSON.parse(value)); } } });
  t.after(() => session.close()); session.start();
  assert.equal(events[1].voice_mode, 'live');
  session.handleMessage({ t: 'ptt', on: true, turn: 4, automatic: true }); assert.equal(starts, 1);
  session.handleMessage({ t: 'ptt', on: true, turn: 5, automatic: true }); assert.equal(starts, 1);
  port.endInput(); port.text('Read this answer'); port.audio(Buffer.alloc(4800)); port.finish(); await sleep(5);
  assert.ok(events.some(e => e.t === 'text' && e.text === 'Read this answer'));
  assert.equal(events.some(e => e.t === 'speak'), false);
});
