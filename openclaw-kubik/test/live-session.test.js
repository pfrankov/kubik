import assert from 'node:assert/strict';
import test from 'node:test';
import { LiveVoiceSession } from '../src/live-voice.js';
import { DeviceSession } from '../src/session.js';
import { parseDeviceMessage } from '../src/protocol.js';
import { fakeEngine, sleep, tone } from './helpers.js';

function fixture(t, { ack = true, cancelAck = true, dispatch, sessionMs, responseMs, onMic } = {}) {
  let callbacks, session, mic = true, playedMs = 0;
  const events = [], calls = { connections: 0, closed: 0, input: 0, dispatch: [] };
  const provider = { createBridge(options) {
    callbacks = options;
    return { connect: async () => { calls.connections++; }, pacesInputAudio: true,
      isConnected: () => true, acknowledgeMark() {}, setMediaTimestamp() {}, submitToolResult() {},
      close: () => { calls.closed++; }, sendAudio: () => { calls.input++; } };
  } };
  const engine = { ...fakeEngine(), voiceMode: 'live', nativeVoice: { available: true },
    createVoiceTurn: options => new LiveVoiceSession({ ...options,
      config: { mode: 'live', provider, cfg: {}, providerConfig: {}, capabilities: {} }, quietMs: 15, sessionMs, responseMs }) };
  session = new DeviceSession({ engine, volume: 40, device: { id: 'desk' }, sessionId: 'authenticated',
    dispatch: dispatch ?? (async request => { calls.dispatch.push(request); await request.deliver('Agent answer.'); }),
    ws: { readyState: 1, bufferedAmount: 0, send(value) {
      if (typeof value !== 'string') {
        assert.equal(mic, true, 'Live closed the physical microphone during output');
        playedMs += (value.length - 5) / 12; return;
      }
      const event = JSON.parse(value); events.push(event);
      if (event.t === 'live_input' && ack) {
        onMic?.(event, callbacks);
        queueMicrotask(() => {
        mic = event.on; session.handleMessage({ t: 'live_input_ack', turn: event.turn, on: event.on });
        });
      }
      if (event.t === 'speak') { assert.equal(mic, true); playedMs = 0; }
      if (event.t === 'speak_cancel' && cancelAck) queueMicrotask(() => session.handleMessage({ t: 'cancelled', gen: event.gen }));
      if (event.t === 'speak_end') queueMicrotask(() => session.handleMessage({ t: 'played', gen: event.gen, ms: playedMs }));
    } } });
  t.after(() => session.close());
  session.start(); session.handleMessage({ t: 'ptt', on: true, turn: 3, automatic: true });
  return { session, callbacks, calls, events };
}

test('Live keeps one SDK connection across quiet and two agent-backed replies; KEY ends it', async t => {
  const { session, callbacks, calls, events } = fixture(t); await sleep(0);
  session.handleAudio({ turn: 3, pcm: Buffer.alloc(1920), reference: Buffer.alloc(1920) });
  await sleep(35); assert.equal(calls.input, 1); assert.equal(calls.closed, 0);
  for (const question of ['First question', 'Second question']) {
    callbacks.onTranscript('user', question, true);
    const result = await callbacks.runAgentConsult({ prompt: question }); assert.equal(result.text, 'Agent answer.');
    callbacks.onAudio(tone(100)); await sleep(45);
    assert.equal(events.at(-1).s, 'listening');
    session.handleAudio({ turn: 3, pcm: Buffer.alloc(1920), reference: Buffer.alloc(1920) });
  }
  assert.equal(calls.connections, 1); assert.equal(calls.dispatch.length, 2); assert.equal(calls.closed, 0);
  assert.ok(calls.dispatch.every(request => request.turnId === 3 && request.isCurrent()));
  assert.equal(events.filter(event => event.t === 'speak').length, 2);
  assert.equal(events.filter(event => event.t === 'input_end').length, 0);
  session.handleMessage({ t: 'cancel', turn: 3 }); await sleep(0);
  assert.equal(calls.closed, 1);
  const input = calls.input; session.handleAudio({ turn: 3, pcm: Buffer.alloc(1920), reference: Buffer.alloc(1920) }); assert.equal(calls.input, input);
});

test('Live replacement waits for the exact cancellation ACK after duplicate clears; idle streams wake again', async t => {
  const { session, callbacks, events, calls } = fixture(t, { cancelAck: false }); await sleep(0);
  callbacks.onAudio(tone(1000)); await sleep(10);
  const first = events.find(event => event.t === 'speak'); assert.ok(first);
  callbacks.onClearAudio('barge-in'); await sleep(0);
  callbacks.onAudio(tone(100)); callbacks.onClearAudio('newer barge-in');
  callbacks.onAudio(tone(200));
  session.handleMessage({ t: 'cancelled', gen: first.gen + 1 }); await sleep(25);
  assert.equal(events.filter(event => event.t === 'speak').length, 1);
  session.handleMessage({ t: 'cancelled', gen: first.gen }); await sleep(70);
  assert.equal(events.filter(event => event.t === 'speak').length, 2);
  assert.equal(events.filter(event => event.t === 'speak_cancel').length, 1);
  assert.equal(events.at(-1).s, 'listening');
  callbacks.onAudio(tone(100)); await sleep(60);
  assert.equal(events.filter(event => event.t === 'speak').length, 3, 'An empty cancelled stream stranded the speech worker');
  assert.equal(calls.closed, 0);
});

test('Live fails once when a physical cancel ACK is missing', async t => {
  const { callbacks, calls, events } = fixture(t, { cancelAck: false }); await sleep(0);
  callbacks.onAudio(tone(1000)); await sleep(10); callbacks.onClearAudio('barge-in');
  callbacks.onAudio(tone(100)); await sleep(3100);
  assert.equal(calls.closed, 1);
  assert.equal(events.filter(event => event.t === 'speak').length, 1);
  assert.equal(events.filter(event => event.t === 'error').length, 1);
});

test('Live unsolicited audio is bounded without an agent consultation', async t => {
  const { callbacks, calls } = fixture(t, { responseMs: 20 }); await sleep(0);
  callbacks.onAudio(tone(1000)); await sleep(35);
  assert.equal(calls.closed, 1);
});

test('Live output waits for its initial ready ACK, ignores a foreign ACK and never requests mic-off', async t => {
  const { session, callbacks, events } = fixture(t, { ack: false }); await sleep(0);
  callbacks.onAudio(tone(100)); await sleep(0);
  assert.equal(events.some(event => event.t === 'speak'), false);
  session.handleMessage({ t: 'live_input_ack', turn: 4, on: true }); await sleep(0);
  assert.equal(events.some(event => event.t === 'speak'), false);
  session.handleMessage({ t: 'live_input_ack', turn: 3, on: true }); await sleep(45);
  assert.equal(events.filter(event => event.t === 'speak').length, 1);
  assert.deepEqual(events.filter(event => event.t === 'live_input').map(event => event.on), [true]);
});

test('Live stop detaches accepted tools and ignores the late response', async t => {
  let release, accepted;
  const pending = new Promise(resolve => { release = resolve; });
  const { session, callbacks, calls, events } = fixture(t, { dispatch: async request => {
    accepted = request; await pending; await request.deliver('Tool finished.');
  } }); await sleep(0);
  const consult = callbacks.runAgentConsult({ prompt: 'Use tools' }); await sleep(0);
  session.handleMessage({ t: 'cancel', turn: 3 });
  assert.equal(accepted.isCurrent(), false); assert.equal(accepted.abortSignal, undefined);
  release(); await assert.rejects(consult, /superseded/); callbacks.onAudio(tone(100)); await sleep(0);
  assert.equal(calls.closed, 1); assert.equal(events.some(event => event.t === 'speak'), false);
});

test('Live session has the SDK 30-minute bound, independently of speech pauses', async t => {
  const { calls, events } = fixture(t, { sessionMs: 20 }); await sleep(35);
  assert.equal(calls.closed, 1); assert.ok(events.some(event => event.t === 'input_end'));
});

test('live microphone ACK validates both the turn and boolean state', () => {
  assert.deepEqual(parseDeviceMessage(JSON.stringify({ t: 'live_input_ack', on: false, turn: 255 })),
    { t: 'live_input_ack', on: false, turn: 255 });
  for (const on of [0, 'false', null]) assert.throws(() => parseDeviceMessage(JSON.stringify({ t: 'live_input_ack', on, turn: 3 })));
  assert.throws(() => parseDeviceMessage(JSON.stringify({ t: 'live_input_ack', on: true, turn: 256 })));
});

test('Live retains bounded late PCM arriving while a reply waits for played', async t => {
  let callbacks, release;
  const audio = [], calls = { resumed: 0, finished: 0 };
  const played = new Promise(resolve => { release = resolve; });
  const voice = new LiveVoiceSession({ turnId: 3, dispatch: async () => {}, quietMs: 10,
    config: { mode: 'live' }, createBridge: options => {
      callbacks = options; return { connect: async () => {}, close() {}, sendAudio() {} };
    }, port: { isCurrent: () => true, pauseInput: async () => {},
      resumeInput: async () => { calls.resumed++; }, audio: pcm => { audio.push(pcm.length); return true; },
      finishReply: async () => { if (++calls.finished === 1) await played; }, text() {}, finish() {},
      fail: reason => assert.fail(reason) } });
  t.after(() => voice.close()); await sleep(0);
  callbacks.audioSink.sendAudio(tone(100)); await sleep(25);
  assert.equal(calls.finished, 1); assert.equal(calls.resumed, 1);
  callbacks.audioSink.sendAudio(tone(200)); release(); await sleep(0);
  assert.deepEqual(audio, [4800, 9600]); assert.equal(calls.resumed, 1);
  await sleep(25); assert.equal(calls.resumed, 1); assert.equal(calls.finished, 2);
});

test('Live continuous transport silence cannot create repeated silent replies or block input', async t => {
  let callbacks;
  const calls = { resumed: 0, finished: 0, input: 0, paused: 0 };
  const voice = new LiveVoiceSession({ turnId: 3, dispatch: async () => {}, quietMs: 10,
    config: { mode: 'live' }, createBridge: options => {
      callbacks = options; return { connect: async () => {}, close() {}, sendAudio() { calls.input++; } };
    }, port: { isCurrent: () => true, pauseInput: async () => { calls.paused++; },
      resumeInput: async () => { calls.resumed++; }, audio: () => true,
      finishReply: async () => { calls.finished++; await sleep(20); }, text() {}, finish() {}, fail: assert.fail } });
  t.after(() => voice.close()); await sleep(0);
  callbacks.audioSink.sendAudio(tone(100));
  const silence = setInterval(() => callbacks.audioSink.sendAudio(Buffer.alloc(960)), 2);
  t.after(() => clearInterval(silence));
  await sleep(85);
  assert.equal(calls.finished, 1, 'transport silence was mistaken for another reply');
  assert.equal(calls.resumed, 1, 'Live unnecessarily reopened its microphone');
  assert.equal(calls.paused, 0, 'silent PCM closed the microphone');
  voice.append(Buffer.alloc(1920), Buffer.alloc(1920)); assert.equal(calls.input, 1, 'next question cannot reach the same bridge');
});


test('Live streams input through speaking and a provider clear cancels output without closing the connection', async t => {
  const { session, callbacks, events, calls } = fixture(t); await sleep(0);
  callbacks.onAudio(tone(1000)); await sleep(10);
  assert.ok(events.some(event => event.t === 'speak'));
  session.handleAudio({ turn: 3, pcm: Buffer.alloc(1920), reference: Buffer.alloc(1920) });
  assert.equal(calls.input, 1);
  callbacks.onClearAudio("barge-in");
  callbacks.onAudio(tone(100)); await sleep(70);
  assert.equal(events.filter(event => event.t === 'speak_cancel').length, 1);
  assert.equal(events.filter(event => event.t === 'speak').length, 2);
  assert.equal(calls.closed, 0);
  assert.deepEqual(events.filter(event => event.t === 'live_input').map(event => event.on), [true]);
  assert.equal(events.at(-1).s, 'listening');
});

test('Live superseded consultation cannot deliver stale text or alias the new prompt', async t => {
  let release;
  const held = new Promise(resolve => { release = resolve; });
  const { callbacks, calls, events } = fixture(t, { dispatch: async request => {
    calls.dispatch.push(request);
    if (request.transcript === 'Old') await held;
    await request.deliver(request.transcript + ' answer');
  } }); await sleep(0);
  const abort = new AbortController();
  const old = callbacks.runAgentConsult({ prompt: 'Old', signal: abort.signal });
  abort.abort();
  const next = await callbacks.runAgentConsult({ prompt: 'New', signal: new AbortController().signal });
  release(); await assert.rejects(old);
  assert.equal(next.text, 'New answer.');
  assert.deepEqual(calls.dispatch.map(request => request.transcript), ['Old', 'New']);
  assert.equal(calls.dispatch[0].isCurrent(), false);
  assert.equal(events.some(event => event.text?.includes('Old answer')), false);
});

test('Live input remains FIFO behind initial readiness ACK and the bridge receives cleaned PCM', async t => {
  let connect, ready;
  const connected = new Promise(resolve => { connect = resolve; });
  const acknowledged = new Promise(resolve => { ready = resolve; });
  const sent = [], raw = tone(40);
  const voice = new LiveVoiceSession({ turnId: 3, dispatch: async () => {}, config: { mode: 'live' },
    createBridge: () => ({ connect: () => connected, sendAudio: pcm => sent.push(Buffer.from(pcm)), close() {} }),
    port: { isCurrent: () => true, resumeInput: () => acknowledged, fail: assert.fail } });
  t.after(() => voice.close());
  voice.append(raw, Buffer.alloc(1920)); connect(); await sleep(0);
  voice.append(Buffer.alloc(1920), Buffer.alloc(1920));
  assert.equal(sent.length, 0, 'Audio bypassed ready ACK');
  ready(); await sleep(0);
  assert.equal(sent.length, 2);
  assert.ok(sent[0].some(value => value !== 0), 'First command was muted');
  assert.notDeepEqual(sent[0], raw, 'Live bypassed its echo/high-pass processing');
  assert.ok(sent[0].reduce((sum, value) => sum + Math.abs(value), 0) > sent[1].reduce((sum, value) => sum + Math.abs(value), 0));
});

test('Overlapping provider clears wait for the first flush before replacement playback', async t => {
  let release;
  const wait = new Promise(resolve => { release = resolve; });
  let callbacks;
  const output = [], calls = { cancels: 0, closes: 0 };
  const voice = new LiveVoiceSession({ turnId: 3, dispatch: async () => {}, quietMs: 10, config: { mode: 'live' },
    createBridge: options => { callbacks = options; return { connect: async () => {}, sendAudio() {}, close() { calls.closes++; } }; },
    port: { isCurrent: () => true, resumeInput: async () => {}, audio: pcm => { output.push(pcm.length); return true; },
      cancelReply: async () => { calls.cancels++; await wait; }, finishReply: async () => {}, fail: assert.fail } });
  t.after(() => voice.close()); await sleep(0);
  callbacks.audioSink.sendAudio(tone(100)); callbacks.audioSink.clearAudio();
  callbacks.audioSink.sendAudio(tone(200)); callbacks.audioSink.clearAudio();
  callbacks.audioSink.sendAudio(tone(300)); await sleep(0);
  assert.deepEqual(output, [4800]);
  release(); await sleep(0);
  assert.deepEqual(output, [4800, 14400]); assert.equal(calls.closes, 0);
});
