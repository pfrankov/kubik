import assert from 'node:assert/strict';
import test from 'node:test';
import WebSocket from 'ws';
import { SPEECH_LEAD_MS } from '../src/protocol.js';
import { sendPayload } from '../src/send.js';
import { channelPlugin } from '../src/channel.js';
import { createEngine } from '../src/engines/index.js';
import { account, connectDevice, DEFAULT_DEVICE_KEY, DEVICE, deviceKey, fakeCore, fakeEngine, fakePairing, mockOpenAI, sleep, tone } from './helpers.js';
import { start, talk, types } from './server-fixture.js';
import { fakeSpeechSdk } from './fake-speech-sdk.js';

test('protocol v1 and v2 hellos are rejected without accepting legacy credentials', async (t) => {
  const { url } = await start(t);
  const oldClient = await connectDevice(url, { hello: { t: 'hello', v: 1, device: DEVICE, token: 'obsolete-token' } });
  assert.equal(await oldClient.closed, 4002);
  assert.equal(oldClient.events.length, 0);
  const v2Client = await connectDevice(url, { hello: { t: 'hello', v: 2, device: DEVICE, key: DEFAULT_DEVICE_KEY.key, fw: '0.6.0' } });
  assert.equal(await v2Client.closed, 4002);
  assert.equal(v2Client.events.length, 0);
});

test('disabled device is rejected', async (t) => {
  const id = deviceKey('other');
  const { url } = await start(t, { pairing: fakePairing({ allowed: [id.entry] }), settings: { devices: { other: { enabled: false } } } });
  const device = await connectDevice(url, { hello: id.hello(), auth: (nonce) => id.sign(nonce) });
  assert.equal(await device.closed, 4001);
});

test('protocol errors close with 4002', async (t) => {
  const { url } = await start(t);
  const binaryFirst = await connectDevice(url, { hello: null });
  binaryFirst.ws.send(Buffer.from([1, 0, 0, 0]));
  assert.equal(await binaryFirst.closed, 4002);
  const garbage = await connectDevice(url, { hello: null });
  garbage.ws.send('{nope');
  assert.equal(await garbage.closed, 4002);
  const ok = await connectDevice(url);
  await ok.waitFor((e) => e.t === 'welcome');
  ok.send({ t: 'ptt', on: 'maybe', turn: 1 });
  assert.equal(await ok.closed, 4002);
  const oversize = await connectDevice(url);
  await oversize.waitFor((e) => e.t === 'welcome');
  oversize.ws.send(JSON.stringify({ t: 'ping', pad: 'x'.repeat(5000) }));
  assert.equal(await oversize.closed, 4002);
});

test('wrong path is 404', async (t) => {
  const { url } = await start(t);
  const ws = new WebSocket(url.replace('/kubik/v1', '/other'));
  const status = await new Promise((resolve) => { ws.on('unexpected-response', (_q, res) => resolve(res.statusCode)); ws.on('error', () => resolve('error')); });
  assert.ok(status === 404 || status === 'error');
});

test('welcome, idle, ping/pong; new connection replaces old (4003); status patches', async (t) => {
  const { url, statuses, server } = await start(t);
  const first = await connectDevice(url);
  assert.deepEqual(await first.waitFor((e) => e.t === 'welcome'), { t: 'welcome', session: 's-1', progress: true, volume: 100 });
  assert.deepEqual(await first.waitFor((e) => e.t === 'capabilities'), { t: 'capabilities', voice_mode: 'classic',
    stt: { available: true }, tts: { available: true } });
  await first.waitFor((e) => e.t === 'state' && e.s === 'idle');
  first.send({ t: 'ping', ts: 42 });
  assert.deepEqual(await first.waitFor((e) => e.t === 'pong'), { t: 'pong', ts: 42 });
  assert.deepEqual(server.onlineDevices, [DEVICE]);
  const second = await connectDevice(url);
  await second.waitFor((e) => e.t === 'welcome');
  assert.equal(await first.closed, 4003);
  await sleep(20);
  assert.deepEqual(server.onlineDevices, [DEVICE]);
  assert.ok(statuses.some((s) => s.lifecycle === 'ready' && s.connected === true));
  assert.ok(statuses.some((s) => Array.isArray(s.onlineDevices) && s.onlineDevices.includes(DEVICE)));
  second.close();
  await second.closed;
  await sleep(20);
  assert.deepEqual(server.onlineDevices, []);
});

test('voice turn: transcript -> agent (exact body, kubik peer) -> speech with emotions', async (t) => {
  const engine = fakeEngine({ transcripts: ['Какая завтра погода?'] });
  const { url, seen } = await start(t, { engine, runtime: { payloads: [
    { text: '[[happy]] Завтра солнечно. [[wink]] Гуляем!' },
    { text: 'Размышление', isReasoning: true },
    { text: 'tool output', kind: 'tool' },
    { text: '**Возьми** зонтик на всякий случай 🌂' },
  ] } });
  const device = await connectDevice(url);
  await device.waitFor((e) => e.t === 'state' && e.s === 'idle');
  talk(device, 1);
  const end = await device.waitFor((e) => e.t === 'speak_end');
  await device.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);
  assert.deepEqual(types(device).sort(), ['challenge', 'welcome', 'capabilities', 'state:idle', 'state:transcribing', 'state:thinking', 'emotion:happy', 'speak',
    'state:speaking', 'emotion:wink', 'speak_end', 'state:idle'].sort());
  // Later emotions are sent at their estimated play time, which may be after speak_end (all audio sent).
  assert.ok(types(device).indexOf('emotion:wink') > types(device).indexOf('speak'));
  const speak = device.events.find((e) => e.t === 'speak');
  assert.deepEqual(speak, { t: 'speak', gen: 1, kind: 'reply' });
  assert.equal(end.gen, 1);
  assert.ok(device.audio.get(1) > 0);
  assert.deepEqual(engine.calls.speak, ['Завтра солнечно.', 'Гуляем!', 'Возьми зонтик на всякий случай.']);
  assert.equal(engine.calls.begin, 1);
  assert.ok(engine.calls.append >= 1);
  const ctx = seen.contexts[0];
  assert.equal(ctx.BodyForAgent, 'Какая завтра погода?');
  assert.equal(ctx.RawBody, 'Какая завтра погода?');
  assert.equal(ctx.Body, `[Kubik kubik:${DEVICE}] Какая завтра погода?`);
  assert.equal(ctx.From, `kubik:${DEVICE}`);
  assert.equal(ctx.To, `kubik:${DEVICE}`);
  assert.equal(ctx.ChatType, 'direct');
  assert.equal(ctx.Provider, 'kubik');
  assert.equal(ctx.Surface, 'kubik');
  assert.equal(ctx.OriginatingChannel, 'kubik');
  assert.equal(ctx.CommandAuthorized, false);
  assert.deepEqual(seen.routes[0].peer, { kind: 'direct', id: DEVICE });
  assert.equal(seen.dispatches[0].replyOptions.disableBlockStreaming, false);
});

test('empty transcript -> stt_empty then idle; agent not called', async (t) => {
  const { url, seen } = await start(t, { engine: fakeEngine({ transcripts: [''] }) });
  const device = await connectDevice(url);
  talk(device, 1);
  await device.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);
  assert.deepEqual(types(device), ['challenge', 'welcome', 'capabilities', 'state:idle', 'state:transcribing', 'error:stt_empty', 'state:idle']);
  assert.equal(seen.dispatches.length, 0);
});

test('stt failure and agent failure report error codes', async (t) => {
  const engine = fakeEngine();
  engine.commit = async () => { throw new Error('boom'); };
  const { url } = await start(t, { engine });
  const device = await connectDevice(url);
  talk(device, 1);
  await device.waitFor((e) => e.t === 'error' && e.code === 'stt_failed');
  await device.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);

  const second = await start(t, { runtime: { dispatchImpl: async () => { throw new Error('agent exploded'); } } });
  const d2 = await connectDevice(second.url);
  talk(d2, 1);
  await d2.waitFor((e) => e.t === 'error' && e.code === 'agent_failed');
  await d2.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);

  const third = await start(t, { runtime: { payloads: [{ text: 'Rate limited', isError: true }] } });
  const d3 = await connectDevice(third.url);
  talk(d3, 1);
  await d3.waitFor((e) => e.t === 'error' && e.code === 'agent_failed');
  await d3.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);
  assert.ok(!d3.events.some((e) => e.t === 'speak'));
});

test('voice failure shows the answer as text and still ends the gen', async (t) => {
  const { url } = await start(t, { engine: fakeEngine({ failSpeak: true }) });
  const device = await connectDevice(url);
  talk(device, 1);
  await device.waitFor((e) => e.t === 'text');
  await device.waitFor((e) => e.t === 'speak_end');
  await device.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);
});

const FW5_ID = DEFAULT_DEVICE_KEY;
const FW5 = { hello: FW5_ID.hello({ fw: '0.6.0' }), auth: (nonce) => FW5_ID.sign(nonce) };

test('firmware 0.6.0 uses IMA ADPCM speech', async (t) => {
  const { url } = await start(t);
  const device = await connectDevice(url, FW5);
  talk(device, 1);
  await device.waitFor((e) => e.t === 'speak_end');
  assert.deepEqual([...device.kinds], [3]);
  assert.ok([...device.audio.values()][0] > 0);
});

test('voice failure on firmware with a screen: the reply is shown as text, no voice_failed', async (t) => {
  const { url } = await start(t, { engine: fakeEngine({ failSpeak: true }),
    runtime: { payloads: [{ text: '[[happy]] Всё готово. Список в заметках.' }] } });
  const device = await connectDevice(url, FW5);
  talk(device, 1);
  const card = await device.waitFor((e) => e.t === 'text');
  assert.equal(card.kind, 'reply');
  assert.match(card.text, /Всё готово\./);
  await device.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);
  assert.ok(!device.events.some((e) => e.t === 'error'));
});

test('[[show]] blocks go to the screen and are not spoken', async (t) => {
  const reply = { text: 'Вот код. [[show]]**ABCD-1234**\n- пункт[[/show]] Скажи, если не подходит.' };
  const { url, engine } = await start(t, { runtime: { payloads: [reply] } });
  const device = await connectDevice(url, FW5);
  talk(device, 1);
  const card = await device.waitFor((e) => e.t === 'text');
  assert.equal(card.text, 'ABCD-1234\n• пункт');
  await device.waitFor((e) => e.t === 'speak_end');
  assert.ok(!engine.calls.speak.join(' ').includes('ABCD'));

});

test('an engine that cannot speak shows every reply as text', async (t) => {
  const engine = { ...fakeEngine(), canSpeak: false };
  const { url } = await start(t, { engine, runtime: { payloads: [{ text: 'Привет! Как дела?' }] } });
  const device = await connectDevice(url, FW5);
  talk(device, 1);
  assert.equal((await device.waitFor((e) => e.t === 'text')).text, 'Привет! Как дела?');
  await device.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);
  assert.ok(!device.events.some((e) => e.t === 'speak'));
});

test('a long reply is synthesised sentence by sentence, the first request short', async (t) => {
  const long = 'Первое предложение ответа достаточно длинное. Второе предложение тоже есть. Третье предложение завершает мысль.';
  const { url, engine } = await start(t, { runtime: { payloads: [{ text: long }] } });
  const device = await connectDevice(url);
  talk(device, 1);
  await device.waitFor((e) => e.t === 'speak_end');
  assert.deepEqual(engine.calls.speak, ['Первое предложение ответа достаточно длинное.',
    'Второе предложение тоже есть. Третье предложение завершает мысль.']);
  assert.equal(device.events.filter((e) => e.t === 'speak').length, 1);
});

test('ptt during speech interrupts: gen bumps, late agent blocks of the old turn are dropped', async (t) => {
  let releaseLate;
  const late = new Promise((r) => { releaseLate = r; });
  let call = 0;
  const engine = fakeEngine({ transcripts: ['Расскажи шутку', 'Привет'], speakDelayMs: 20 });
  const { url, logs } = await start(t, { engine, runtime: { dispatchImpl: async ({ dispatcherOptions }) => {
    call++;
    if (call === 1) {
      await dispatcherOptions.deliver({ text: '[[joy]] ' + 'Очень длинная шутка. '.repeat(20) }, { kind: 'block' });
      await late;
      await dispatcherOptions.deliver({ text: 'Поздний блок старого хода.' }, { kind: 'final' });
    } else {
      await dispatcherOptions.deliver({ text: '[[happy]] Привет!' }, { kind: 'final' });
    }
  } } });
  const device = await connectDevice(url);
  talk(device, 1);
  await device.waitFor((e) => e.t === 'speak' && e.gen === 1);
  await sleep(100);
  talk(device, 2);
  const speak2 = await device.waitFor((e) => e.t === 'speak' && e.gen === 2);
  assert.equal(speak2.kind, 'reply');
  releaseLate();
  await device.waitFor((e) => e.t === 'speak_end' && e.gen === 2);
  await sleep(50);
  assert.ok(!engine.calls.speak.includes('Поздний блок старого хода.'));
  assert.ok(!device.events.some((e) => e.t === 'speak_end' && e.gen === 1), 'interrupted gen gets no speak_end');
  assert.ok(logs.some((l) => /superseded turn 1 not spoken/.test(l)), logs.join('\n'));
  assert.ok(engine.calls.cancel >= 1);
});

test('cancel stops speech and returns to idle', async (t) => {
  const engine = fakeEngine({ speakDelayMs: 30 });
  const { url } = await start(t, { engine, runtime: { payloads: [{ text: 'Длинный ответ. '.repeat(30) }] } });
  const device = await connectDevice(url, { autoPlayed: false });
  talk(device, 1);
  const speak = await device.waitFor((e) => e.t === 'speak');
  await sleep(100);
  device.send({ t: 'cancel', gen: speak.gen });
  await device.waitForNth((e) => e.t === 'state' && e.s === 'idle', 2);
  const bytes = device.audio.get(speak.gen);
  await sleep(200);
  assert.equal(device.audio.get(speak.gen), bytes, 'no audio after cancel');
  assert.ok(!device.events.some((e) => e.t === 'speak_end'));
});

test('foreign input is ignored; real transport requires consumed-audio credit before refilling', async (t) => {
  const engine = fakeEngine({ speakDelayMs: 0 });
  const { url } = await start(t, { engine, runtime: { payloads: [{ text: 'А'.repeat(3000) }] } });
  const device = await connectDevice(url, { autoPlayed: false, autoProgress: false });
  device.send({ t: 'ptt', on: true, turn: 1 });
  device.sendAudio(9, tone(200));
  await sleep(30);
  assert.equal(engine.calls.append, 0);
  device.sendAudio(1, tone(200));
  device.send({ t: 'ptt', on: false, turn: 1 });
  const speak = await device.waitFor((e) => e.t === 'speak');
  await sleep(150);
  assert.equal(device.receivedMs(speak.gen), SPEECH_LEAD_MS);
  await sleep(150);
  assert.equal(device.receivedMs(speak.gen), SPEECH_LEAD_MS, 'elapsed time must not grant unconfirmed credit');
  device.send({ t: 'progress', gen: speak.gen, ms: 600 });
  await sleep(150);
  assert.equal(device.receivedMs(speak.gen), SPEECH_LEAD_MS + 600, 'refill matches consumed duration');
});

test('outbound: offline notification is queued, not-running errors are clear; notify speaks with kind=notify', async (t) => {
  await assert.rejects(sendPayload(`kubik:${DEVICE}`, { text: 'Привет' }, { accountId: 'default' }),
    (error) => error.code === 'KUBIK_DELIVERY_FAILED' && /not running/.test(error.message));
  const { url } = await start(t);
  const queued = await channelPlugin.outbound.sendText({ to: `kubik:${DEVICE}`, text: 'Привет', accountId: 'default' });
  assert.equal(queued.meta.deliveryStatus, 'queued');
  assert.equal(queued.meta.spokenChars, 0);
  await assert.rejects(channelPlugin.outbound.sendText({ to: 'kubik:nope nope', text: 'x' }), /Invalid Kubik target/);
  await assert.rejects(channelPlugin.outbound.sendText({ to: `kubik:${DEVICE}`, text: '😀 🙂' }), /Nothing to say or show/);

  const device = await connectDevice(url);
  await device.waitFor((e) => e.t === 'state' && e.s === 'idle');
  const result = await channelPlugin.outbound.sendText({ to: `kubik:${DEVICE}`, text: '[[proud]] Пора пить воду!', accountId: 'default' });
  assert.equal(result.channel, 'kubik');
  assert.deepEqual(result.target, { kind: 'chat', id: DEVICE });
  assert.ok(result.meta.spokenChars > 0);
  assert.equal(result.meta.deliveryStatus, 'played');
  const speak = await device.waitFor((e) => e.t === 'speak');
  assert.equal(speak.kind, 'notify');
  await device.waitFor((e) => e.t === 'speak_end' && e.gen === speak.gen);
  assert.ok(types(device).includes('emotion:proud'));
  assert.ok(device.audio.get(speak.gen) > 0);
  const media = await channelPlugin.outbound.sendPayload({ to: DEVICE, payload: { text: 'Смотри', mediaUrl: 'https://x/y.png' }, accountId: 'default' });
  assert.ok(media.meta.spokenChars > 0);
});

test('notify waits for the reply in progress and uses the next gen', async (t) => {
  const engine = fakeEngine({ speakDelayMs: 10 });
  const { url } = await start(t, { engine, runtime: { payloads: [{ text: 'Ответ на вопрос. '.repeat(5) }] } });
  const device = await connectDevice(url);
  talk(device, 1);
  await device.waitFor((e) => e.t === 'speak');
  const notified = channelPlugin.outbound.sendText({ to: DEVICE, text: 'Напоминание!', accountId: 'default' });
  await notified;
  const speaks = device.events.filter((e) => e.t === 'speak');
  assert.deepEqual(speaks.map((e) => [e.gen, e.kind]), [[1, 'reply'], [2, 'notify']]);
  const order = device.events.map((e) => `${e.t}:${e.gen ?? ''}`);
  assert.ok(order.indexOf('speak_end:1') < order.indexOf('speak:2'));
});

test('disconnect while notifying rejects delivery as mayHaveSent', async (t) => {
  const engine = fakeEngine({ speakDelayMs: 50 });
  const { url } = await start(t, { engine });
  const device = await connectDevice(url);
  await device.waitFor((e) => e.t === 'state');
  const pending = channelPlugin.outbound.sendText({ to: DEVICE, text: 'Длинное сообщение. '.repeat(20), accountId: 'default' });
  await device.waitFor((e) => e.t === 'speak');
  device.ws.terminate();
  await assert.rejects(pending, (error) => /disconnected/.test(error.message) && error.mayHaveSent === true);
});

test('full stack: server + openai-http engine + mock OpenAI', async (t) => {
  const mock = await mockOpenAI(t);
  const { url, seen } = await start(t, { settings: { voice: { provider: 'openai-http', baseUrl: mock.url, apiKey: 'sk-test-DO-NOT-LOG' } },
    engineFactory: createEngine, runtime: { payloads: [{ text: '[[happy]] Привет! Всё отлично.' }] } });
  const device = await connectDevice(url);
  talk(device, 1, 1000);
  const speak = await device.waitFor((e) => e.t === 'speak', 10000);
  await device.waitFor((e) => e.t === 'speak_end', 10000);
  assert.equal(seen.contexts[0].BodyForAgent, 'Привет! Как дела?');
  assert.ok(device.audio.get(speak.gen) > 12 * 500);
});

test('full stack: server + openclaw engine (core STT/TTS)', async (t) => {
  const { core, calls } = fakeCore({ transcript: 'Который час?' });
  const { url, seen } = await start(t, { settings: { voice: { provider: 'openclaw' },
    tts: { provider: 'openai', providers: { openai: { apiKey: 'test-key' } } } },
    engineFactory: (voice, options) => createEngine(voice, { ...options, speechSdk: fakeSpeechSdk(core) }),
    runtime: { payloads: [{ text: '[[happy]] Сейчас полдень.' }], extraCore: core } });
  const device = await connectDevice(url);
  talk(device, 1, 800);
  const speak = await device.waitFor((e) => e.t === 'speak', 5000);
  await device.waitFor((e) => e.t === 'speak_end', 5000);
  assert.equal(seen.contexts[0].BodyForAgent, 'Который час?');
  assert.equal(calls.stt[0].sampleRate, 24000);
  assert.equal(calls.tts[0].text, 'Сейчас полдень.');
  assert.ok(types(device).includes('emotion:happy'));
  assert.ok(device.audio.get(speak.gen) >= 12 * 500 && device.audio.get(speak.gen) < 12 * 500 + 40);
});
