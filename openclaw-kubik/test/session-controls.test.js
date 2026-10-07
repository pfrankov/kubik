import assert from 'node:assert/strict';
import test from 'node:test';
import { DeviceSession } from '../src/session.js';
import { fakeEngine, sleep } from './helpers.js';

function fixture(t, options = {}) {
  const events = [];
  const engine = options.engine ?? fakeEngine();
  let session;
  let playedSamples = 0;
  const ws = { readyState: 1, bufferedAmount: 0, close() { this.readyState = 3; }, send(value) {
    options.beforeSend?.(value);
    if (typeof value !== 'string') {
      playedSamples += Math.max(0, value.length - 5) * 2;
      const progress = { t: 'progress', gen: session.activeGen, ms: Math.round(playedSamples / 24) };
      queueMicrotask(() => session.handleMessage(progress));
      return;
    }
    const event = JSON.parse(value);
    events.push(event);
    if (event.t === 'speak') playedSamples = 0;
    if (event.t === 'text' && event.receipt) queueMicrotask(() => session.handleMessage({ t: 'shown', receipt: event.receipt }));
    if (event.t === 'speak_end') queueMicrotask(() => session.handleMessage({ t: 'played', gen: event.gen, ms: Math.round(playedSamples / 24) }));
  } };
  session = new DeviceSession({ ws, device: { id: 'desk' }, engine, volume: 100,
    dispatch: async ({ deliver }) => deliver('Ответ. Ещё предложение.'), ...options });
  t.after(() => session.close());
  return { session, engine, events };
}

test('automatic PTT rejects a crossed notification; manual KEY can still interrupt it', async (t) => {
  let release;
  const speaking = new Promise(resolve => { release = resolve; });
  const engine = fakeEngine();
  engine.speak = async () => speaking;
  const { session, events } = fixture(t, { engine });
  const notification = session.notify('Still speaking.'); await sleep(10);
  const epoch = session.epoch, gen = session.activeGen;
  assert.ok(gen != null);
  session.handleMessage({ t: 'ptt', on: true, turn: 9, automatic: true });
  assert.equal(session.epoch, epoch); assert.equal(session.activeGen, gen);
  assert.equal(engine.calls.begin, 0); assert.ok(events.some(e => e.t === 'error' && e.code === 'busy'));
  session.handleMessage({ t: 'cancel', turn: 9 }); // rejected start cannot cancel notification
  assert.equal(session.activeGen, gen); assert.equal(engine.calls.cancel, 0);
  session.handleMessage({ t: 'ptt', on: true, turn: 9 });
  assert.equal(engine.calls.begin, 1); assert.equal(session.activeGen, null);
  release({ cancelled: true }); await notification;
});

test('turn-scoped cancellation releases recording and queued notification without STT or speech cancellation', async (t) => {
  const { session, engine } = fixture(t);
  session.handleMessage({ t: 'ptt', on: true, turn: 255, automatic: true });
  session.handleAudio({ turn: 255, pcm: Buffer.alloc(4800) });
  const notification = session.notify('A queued reminder.'); await sleep(10);
  assert.equal(engine.calls.speak.length, 0);
  session.handleMessage({ t: 'cancel', turn: 254 });
  assert.equal(engine.calls.cancelTranscription, 0);
  session.handleMessage({ t: 'cancel', turn: 255 });
  const result = await notification;
  assert.equal(result.status, 'played'); assert.equal(engine.calls.commit, 0);
  assert.equal(engine.calls.cancelTranscription, 1); assert.equal(engine.calls.cancel, 1); // PTT-on only
});

test('a cancelled automatic transcription cannot dispatch after its promise settles', async (t) => {
  let release; let dispatched = 0;
  const engine = fakeEngine(); engine.commit = () => new Promise(resolve => { release = resolve; });
  const { session } = fixture(t, { engine, dispatch: async () => { dispatched++; } });
  session.handleMessage({ t: 'ptt', on: true, turn: 3, automatic: true });
  session.handleMessage({ t: 'ptt', on: false, turn: 3 });
  session.handleMessage({ t: 'cancel', turn: 3 }); release('Late transcript.'); await sleep(10);
  assert.equal(dispatched, 0); assert.equal(engine.calls.cancelTranscription, 1);
});

for (const volume of [0, 19, 20]) {
  test(`voice output threshold: volume ${volume}`, async (t) => {
    const { session, engine, events } = fixture(t, { volume });
    const result = await session.notify('Полезный ответ.');
    assert.equal(engine.calls.speak.length, volume >= 20 ? 1 : 0);
    assert.equal(result.status, volume >= 20 ? 'played' : 'shown');
    assert.equal(events.some((event) => event.t === 'text'), volume < 20);
  });
}

test('local volume change controls the next notification; missing TTS still shows text', async (t) => {
  const { session, engine, events } = fixture(t);
  session.handleMessage({ t: 'device_state', volume: 19 });
  await session.notify('Тихий ответ.');
  session.handleMessage({ t: 'device_state', volume: 20 });
  engine.canSpeak = false;
  await session.notify('Озвучка не настроена.');
  assert.equal(engine.calls.speak.length, 0);
  assert.equal(events.filter((event) => event.t === 'text').length, 2);
});

test('missing STT never starts audio processing; gives an actionable text message', async (t) => {
  const engine = { ...fakeEngine(), canListen: false };
  const { session, events } = fixture(t, { engine });
  session.handleMessage({ t: 'ptt', on: true, turn: 1 });
  session.handleAudio({ turn: 1, pcm: Buffer.alloc(4800) });
  session.handleMessage({ t: 'ptt', on: false, turn: 1 });
  await sleep(10);
  assert.equal(engine.calls.begin, 0);
  assert.equal(engine.calls.append, 0);
  assert.equal(engine.calls.commit, 0);
  assert.equal(events.find((event) => event.t === 'text').text,
    'Speech input is not available yet. Open Settings, then Agent, to check STT.');
});

test('agent picker paginates; canonical selection is acknowledged; concurrency is bounded', async (t) => {
  let chosen = 'provider/m0';
  let release;
  const wait = new Promise((resolve) => { release = resolve; });
  const models = Array.from({ length: 9 }, (_, index) => ({ id: `provider/m${index}`, label: `Модель ${index}` }));
  const agentControl = { options: async () => ({ model: chosen, models, stt: { available: true }, tts: { available: false } }),
    selectModel: async (_device, id) => { await wait; chosen = id; } };
  const { session, events } = fixture(t, { agentControl });
  await session.handleMessage({ t: 'agent_options', target: 'agent', rid: 1, cursor: 1 });
  assert.deepEqual(events.at(-1).models, models.slice(4, 8));
  const selecting = session.handleMessage({ t: 'agent_model', target: 'agent', rid: 2, cursor: 1, id: 'provider/m5' });
  session.handleMessage({ t: 'ptt', on: true, turn: 1 });
  assert.equal(events.at(-1).code, 'busy', 'recording waits for model selection');
  await session.handleMessage({ t: 'agent_options', target: 'agent', rid: 3, cursor: 2 });
  assert.equal(events.at(-1).error, 'busy');
  assert.equal(events.at(-1).cursor, 2);
  assert.equal(events.at(-1).total, 0);
  release();
  await selecting;
  assert.equal(events.at(-1).rid, 2);
  assert.equal(events.at(-1).cursor, 1, 'selection acknowledgement matches the selected page');
  assert.equal(events.at(-1).total, 9);
  assert.deepEqual(events.at(-1).models, models.slice(4, 8));
  assert.equal(events.at(-1).model, 'provider/m5');
});

test('model changes stay busy across reconnect and timeout until the shared operation settles', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  let releaseOld;
  let markOldStarted;
  const oldStarted = new Promise((resolve) => { markOldStarted = resolve; });
  const oldGate = new Promise((resolve) => { releaseOld = resolve; });
  t.after(releaseOld);
  const chosen = new Map([['desk', 'model-0'], ['other', 'model-0']]);
  const models = ['model-0', 'model-1', 'model-2'].map(id => ({ id, label: id }));
  const agentControl = {
    async options(device) { return { model: chosen.get(device.id) ?? '', models }; },
    async selectModel(device, id) {
      if (device.id === 'desk' && id === 'model-1') { markOldStarted(); await oldGate; }
      chosen.set(device.id, id);
    },
  };

  const old = fixture(t, { agentControl });
  const oldSelection = old.session.handleMessage({ t: 'agent_model', target: 'agent', rid: 1, cursor: 0, id: 'model-1' });
  await oldStarted;
  assert.equal(old.session.controls.changingModel, true);
  t.mock.timers.tick(8000);
  await oldSelection;
  assert.equal(old.events.at(-1).error, 'timeout');
  old.session.close();

  const current = fixture(t, { agentControl });
  assert.equal(current.session.controls.changingModel, true, 'the new session observes the outstanding write');
  await current.session.handleMessage({ t: 'agent_model', target: 'agent', rid: 2, cursor: 0, id: 'model-2' });
  assert.equal(current.events.at(-1).error, 'busy');
  assert.equal(chosen.get('desk'), 'model-0', 'the new selection was not allowed to race the old write');

  const otherDevice = fixture(t, { agentControl, device: { id: 'other' } });
  await otherDevice.session.handleMessage({ t: 'agent_model', target: 'agent', rid: 3, cursor: 0, id: 'model-2' });
  assert.equal(otherDevice.events.at(-1).model, 'model-2', 'another device has an independent control lane');

  releaseOld();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(chosen.get('desk'), 'model-1');
  assert.equal(current.session.controls.changingModel, false, 'the lane unlocks when the underlying write settles');
  await current.session.handleMessage({ t: 'agent_model', target: 'agent', rid: 4, cursor: 0, id: 'model-2' });
  assert.equal(current.events.at(-1).model, 'model-2');
  assert.equal(chosen.get('desk'), 'model-2');
});

test('changing model during a recorded request is rejected without calling the adapter', async (t) => {
  let changed = false;
  const agentControl = { options: async () => ({}), selectModel: async () => { changed = true; } };
  const { session, events } = fixture(t, { agentControl });
  session.handleMessage({ t: 'ptt', on: true, turn: 2 });
  await session.handleMessage({ t: 'agent_model', target: 'agent', rid: 1, cursor: 0, id: 'provider/x' });
  assert.equal(events.at(-1).error, 'busy');
  assert.equal(events.at(-1).cursor, 0);
  assert.equal(events.at(-1).total, 0);
  assert.equal(changed, false);
});

test('model selection errors echo the requested page and include an empty catalog count', async (t) => {
  const agentControl = { options: async () => ({}), selectModel: async () => { throw Object.assign(new Error(), { code: 'invalid_model' }); } };
  const { session, events } = fixture(t, { agentControl });
  await session.handleMessage({ t: 'agent_model', target: 'agent', rid: 4, cursor: 2, id: 'provider/missing' });
  assert.deepEqual(events.at(-1), { t: 'agent_options', target: 'agent', rid: 4, cursor: 2, total: 0, error: 'invalid_model' });
});

async function waitUntil(check) {
  for (let attempt = 0; attempt < 400; attempt++) {
    if (check()) return;
    await sleep(5);
  }
  assert.fail('Session did not reach the expected state');
}

function blockedReply(t, options = {}) {
  const engine = fakeEngine({ speakDelayMs: 0 });
  const speak = engine.speak.bind(engine);
  let release, entered = false, accepted = 0, finished = false;
  const gate = new Promise((resolve) => { release = resolve; });
  engine.speak = async (text, options) => {
    if (!entered) {
      entered = true;
      options.signal.addEventListener('abort', release, { once: true });
      try { await gate; }
      finally { options.signal.removeEventListener('abort', release); }
    }
    return speak(text, options);
  };
  const phrases = Array.from({ length: 50 }, (_, i) => `Phrase ${i}.`);
  const result = fixture(t, { engine, ...options, dispatch: async ({ deliver }) => {
    for (const phrase of phrases) if (await deliver(phrase)) accepted++;
    finished = true;
  } });
  t.after(release);
  result.session.handleMessage({ t: 'ptt', on: true, turn: 7 });
  result.session.handleAudio({ turn: 7, pcm: Buffer.alloc(4800) });
  result.session.handleMessage({ t: 'ptt', on: false, turn: 7 });
  return { ...result, phrases, release, accepted: () => accepted, finished: () => finished };
}

test('worker failure rejects a blocked producer and closes the broken connection', async (t) => {
  let failSend = false;
  const run = blockedReply(t, { beforeSend(value) {
    if (failSend && typeof value === 'string' && JSON.parse(value).t === 'text') throw Error('send failed');
  } });
  await waitUntil(() => run.accepted() === 33);
  failSend = true;
  run.engine.speak = async () => { throw Error('TTS unavailable'); };
  run.release();
  await waitUntil(run.finished);
  assert.equal(run.session.closed, true);
  assert.equal(run.session.ws.readyState, 3);
  assert.ok(run.accepted() < 50, 'worker failure releases and rejects remaining delivery');
});

test('notification authorization failure releases a full queue and rejects delivery', async (t) => {
  let rejectAuthorization, calls = 0;
  const gate = new Promise((_resolve, reject) => { rejectAuthorization = reject; });
  const { session, engine } = fixture(t, { authorize: async () => ++calls === 1 ? true : gate });
  const delivery = session.notify('A complete phrase to synthesize. '.repeat(250));
  const rejected = assert.rejects(delivery, /authorization unavailable/);
  await waitUntil(() => calls === 2);
  await sleep(20); // producer fills its queue while authorization is suspended
  rejectAuthorization(Error('authorization unavailable'));
  await rejected;
  assert.equal(session.closed, true);
  assert.equal(engine.calls.speak.length, 0);
});

test('slow TTS bounds the real session queue without losing or reordering reply phrases', async (t) => {
  const run = blockedReply(t);
  await waitUntil(() => run.accepted() === 33); // one synthesizing, 32 waiting
  await sleep(20);
  assert.equal(run.accepted(), 33);
  assert.equal(run.finished(), false);
  run.release();
  await waitUntil(() => run.events.some((event) => event.t === 'speak_end'));
  assert.equal(run.finished(), true);
  assert.deepEqual(run.engine.calls.speak, run.phrases);
  assert.equal(run.events.filter((event) => event.t === 'speak_end').length, 1);
});

for (const action of ['interrupt', 'close']) {
  test(`${action} releases a producer blocked by slow TTS`, async (t) => {
    const run = blockedReply(t);
    await waitUntil(() => run.accepted() === 33);
    run.session[action]('test');
    await waitUntil(run.finished);
    assert.equal(run.accepted(), 33, 'late and blocked phrases are rejected');
    assert.ok(run.engine.calls.speak.length <= 1, 'queued phrases are not spoken');
    assert.equal(run.events.filter((event) => event.t === 'speak_end').length, 0);
  });
}


test('mode save confirms the catalog before advertising capabilities; failed readback emits no capability change', async (t) => {
  let mode = 'classic', failRead = false;
  const engine = { ...fakeEngine(), voiceMode: mode,
    async refreshCapabilities() { this.voiceMode = mode; } };
  const agentControl = {
    async selectModel(_device, id, target) { assert.equal(target, 'mode'); mode = id; },
    async options(_device, target) {
      assert.equal(target, 'mode'); if (failRead) throw new Error('Readback unavailable');
      return { model: mode, models: [{ id: mode, label: mode, available: true }],
        stt: { available: true }, tts: { available: true } };
    },
  };
  const { session, events } = fixture(t, { engine, agentControl });
  session.handleMessage({ t: 'agent_model', target: 'mode', rid: 31, cursor: 0, id: 'realtime' });
  await sleep(20);
  const ack = events.findIndex(e => e.t === 'agent_options' && e.rid === 31);
  const caps = events.findIndex(e => e.t === 'capabilities');
  assert.ok(ack >= 0 && caps > ack); assert.equal(events[caps].voice_mode, 'realtime');
  events.length = 0; failRead = true;
  session.handleMessage({ t: 'agent_model', target: 'mode', rid: 32, cursor: 0, id: 'live' });
  await sleep(20);
  assert.equal(events.find(e => e.t === 'agent_options').error, 'unavailable');
  assert.equal(events.some(e => e.t === 'capabilities'), false);
});
