import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import test from 'node:test';
import { StreamPitch } from '../src/stream-pitch.js';
import { streamDeviceSpeech } from '../src/stream-speech.js';
import { LiveTranscription, resolveLiveCredential } from '../src/live-transcription.js';
import { pitchShiftPcm16 } from '../src/audio.js';
import { OpenClawEngine, toDevicePcm } from '../src/engines/openclaw.js';

test('live transcription selects the configured Platform key even when chat prefers OAuth', async () => {
  let calls = 0;
  const resolveAuth = async () => { calls++; return { mode: 'oauth', apiKey: 'subscription-token' }; };
  assert.equal(await resolveLiveCredential({ env: { OPENAI_API_KEY: 'platform-key' }, resolveAuth }), 'platform-key');
  assert.equal(calls, 0);
  await assert.rejects(resolveLiveCredential({ env: {}, resolveAuth }), /configured OpenAI API key/);
  assert.equal(await resolveLiveCredential({ env: {}, resolveAuth: async () => ({ mode: 'api-key', apiKey: 'profile-key' }) }), 'profile-key');
  await assert.rejects(resolveLiveCredential({ env: {}, resolveAuth: async () => ({ mode: 'token', apiKey: 'token' }) }), /configured OpenAI API key/);
});

function tone(ms = 3000) {
  const pcm = Buffer.alloc(ms * 48);
  for (let i = 0; i < pcm.length / 2; i++) pcm.writeInt16LE(Math.round(8000 * Math.sin(2 * Math.PI * 150 * i / 24000)), i * 2);
  return pcm;
}
test('stream pitch has exact duration, shifted frequency and identical output across arbitrary chunk boundaries', () => {
  const input = tone(), runs = [];
  for (const split of [4095, 9600, input.length]) {
    const dsp = new StreamPitch(6), chunks = [];
    for (let i = 0; i < input.length; i += split) chunks.push(dsp.push(input.subarray(i, i + split)));
    chunks.push(dsp.finish()); runs.push(Buffer.concat(chunks));
  }
  assert.equal(runs[0].length, input.length);
  assert.deepEqual(runs[0], runs[1]); assert.deepEqual(runs[0], runs[2]);
  const reference = pitchShiftPcm16(input, 6);
  for (let i = 0; i < reference.length; i += 2) assert.ok(Math.abs(runs[0].readInt16LE(i) - reference.readInt16LE(i)) <= 1);
  let crossings = 0;
  for (let i = 24001; i < 48000; i++) if ((runs[0].readInt16LE(i * 2 - 2) < 0) !== (runs[0].readInt16LE(i * 2) < 0)) crossings++;
  assert.ok(Math.abs(crossings / 2 - 212) < 8);
});

function ttsSdk(body, calls = []) {
  return { resolveTtsConfig: () => ({}), getTtsProvider: () => 'openai', resolveTtsPrefsPath: () => '/unused',
    getResolvedSpeechProviderConfig: () => ({ apiKey: 'test-key', baseUrl: 'https://api.openai.com/v1', model: 'configured-model', voice: 'configured-voice', instructions: 'configured-style' }),
    resolveProviderHttpRequestConfig: (params) => { calls.push(params); return { headers: params.defaultHeaders }; },
    postJsonRequest: async (params) => { calls.push(params); return { response: { ok: true, body }, release: async () => calls.push('release') }; } };
}
test('configured TTS delivers PCM before synthesis finishes and releases provider resource', async () => {
  let unblock, first;
  const blocked = new Promise((resolve) => { unblock = resolve; });
  const output = new Promise((resolve) => { first = resolve; });
  const calls = [];
  async function* body() { yield tone(200); await blocked; yield tone(200); }
  let complete = false;
  const request = { allowPrivateNetwork: true };
  const pending = streamDeviceSpeech({ cfg: { models: { providers: { openai: { request } } } }, text: 'Hello.', pitch: 6, signal: new AbortController().signal,
    timeoutMs: 2000, onAudio: first, loadSdk: async () => ttsSdk(body(), calls) }).then(() => { complete = true; });
  const pcm = await output;
  assert.ok(pcm.length > 0); assert.equal(complete, false);
  assert.deepEqual(calls[0].request, request, 'explicit provider request policy must reach the guarded SDK');
  assert.equal(calls[0].allowPrivateNetwork, true, 'only explicit provider request policy permits private hosts');
  assert.deepEqual(calls[1].body, { model: 'configured-model', voice: 'configured-voice', input: 'Hello.', response_format: 'pcm', instructions: 'configured-style' });
  unblock(); await pending; assert.equal(calls.at(-1), 'release');
});
test('TTS leaves private-network guard enabled without explicit operator policy', async () => {
  for (const request of [undefined, {}, { allowPrivateNetwork: false }]) {
    const calls = [];
    await streamDeviceSpeech({ cfg: { models: { providers: { openai: { request } } } }, text: 'x',
      pitch: 6, signal: new AbortController().signal, timeoutMs: 1000,
      loadSdk: async () => ttsSdk([tone(200)], calls) });
    assert.equal(calls[0].allowPrivateNetwork, false);
  }
});
test('unsupported configured streaming provider fails without retrying another provider', async () => {
  let params;
  const sdk = ttsSdk([]); sdk.getTtsProvider = () => 'elevenlabs';
  sdk.streamSpeech = async (input) => { params = input; return { success: false, release: async () => {} }; };
  await assert.rejects(streamDeviceSpeech({ cfg: {}, text: 'x', signal: new AbortController().signal, timeoutMs: 1000,
    loadSdk: async () => sdk }), /configured provider/);
  assert.equal(params.disableFallback, true); assert.equal(params.overrides.provider, 'elevenlabs');
});
test('cancelled streaming startup releases the late provider resource and emits no audio', async () => {
  let releaseRequest, releases = 0, audio = 0;
  const sdk = ttsSdk([]); sdk.getTtsProvider = () => 'elevenlabs';
  sdk.streamSpeech = () => new Promise((resolve) => { releaseRequest = resolve; });
  const controller = new AbortController();
  const pending = streamDeviceSpeech({ cfg: {}, text: 'x', signal: controller.signal, timeoutMs: 1000,
    onAudio: () => { audio++; }, loadSdk: async () => sdk });
  await new Promise((resolve) => setImmediate(resolve)); controller.abort();
  await assert.rejects(pending, { name: 'AbortError' });
  releaseRequest({ success: true, audioStream: [], outputFormat: 'pcm_24000', release: async () => { releases++; } });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(releases, 1); assert.equal(audio, 0);
});
test('non-streaming provider uses exactly its own telephony method; failure never invokes another provider', async () => {
  const sdk = ttsSdk([]); sdk.getTtsProvider = () => 'custom';
  let calls = 0;
  sdk.getSpeechProvider = (name) => {
    assert.equal(name, 'custom');
    return { synthesizeTelephony: async () => { calls++; return { audioBuffer: tone(300), sampleRate: 24000, outputFormat: 'pcm' }; } };
  };
  let bytes = 0;
  await streamDeviceSpeech({ cfg: {}, text: 'x', signal: new AbortController().signal, timeoutMs: 1000, pitch: 6,
    onAudio: (pcm) => { bytes += pcm.length; }, normalizePcm: toDevicePcm, loadSdk: async () => sdk });
  assert.equal(calls, 1); assert.equal(bytes, 48 * 300);
  sdk.getSpeechProvider = () => ({ synthesizeTelephony: async () => { calls++; throw new Error('provider failed'); } });
  await assert.rejects(streamDeviceSpeech({ cfg: {}, text: 'x', signal: new AbortController().signal, timeoutMs: 1000,
    normalizePcm: toDevicePcm, loadSdk: async () => sdk }), /provider failed/);
  assert.equal(calls, 2);
});

class Socket extends EventEmitter {
  static last;
  constructor(url, opts) { super(); Object.assign(this, { url, opts, readyState: 1, bufferedAmount: 0, sent: [] }); Socket.last = this; }
  send(data) { this.sent.push(JSON.parse(data)); }
  terminate() { this.closed = true; this.emit('close'); }
  event(value) { this.emit('message', JSON.stringify(value)); }
}
const tick = () => new Promise((resolve) => setImmediate(resolve));
async function live(options = {}) {
  const turn = new LiveTranscription({ cfg: {}, credential: async () => 'test-key', Socket, ...options });
  await tick(); Socket.last.emit('open'); return { turn, socket: Socket.last };
}
test('live STT sends audio during PTT, waits for commit, returns only matching final transcript and closes', async () => {
  const { turn, socket } = await live();
  turn.append(tone(100)); socket.event({ type: 'session.updated' });
  assert.equal(socket.sent[0].session.audio.input.transcription.model, 'gpt-live-transcribe');
  assert.equal(socket.sent[0].session.audio.input.turn_detection, null);
  assert.equal(socket.sent[1].type, 'input_audio_buffer.append');
  assert.equal(socket.sent.some((e) => e.type === 'input_audio_buffer.commit'), false);
  turn.append(tone(100)); const pending = turn.commit();
  assert.equal(socket.sent.at(-1).type, 'input_audio_buffer.commit');
  socket.event({ type: 'conversation.item.input_audio_transcription.delta', delta: 'partial' });
  socket.event({ type: 'conversation.item.input_audio_transcription.completed', item_id: 'old', transcript: 'obsolete' });
  socket.event({ type: 'input_audio_buffer.committed', item_id: 'current' });
  socket.event({ type: 'conversation.item.input_audio_transcription.completed', item_id: 'current', transcript: ' Привет ' });
  assert.equal(await pending, 'Привет'); assert.equal(socket.closed, true);
  socket.event({ type: 'error' });
});
test('live STT commits after readiness, accepts completion before commit acknowledgment', async () => {
  const { turn, socket } = await live(); turn.append(tone(150)); const pending = turn.commit();
  socket.event({ type: 'session.updated' });
  assert.deepEqual(socket.sent.slice(1).map((v) => v.type), ['input_audio_buffer.append', 'input_audio_buffer.commit']);
  socket.event({ type: 'conversation.item.input_audio_transcription.completed', item_id: 'current', transcript: 'ok' });
  socket.event({ type: 'input_audio_buffer.committed', item_id: 'current' });
  assert.equal(await pending, 'ok');
});
test('live STT closes on cancellation and fails bounded startup buffers without batch fallback', async () => {
  const { turn, socket } = await live(); turn.append(tone(150)); const pending = turn.commit(); turn.cancel();
  await assert.rejects(pending, { name: 'AbortError' }); assert.equal(socket.closed, true);
  const next = await live(); next.turn.append(Buffer.alloc(1024 * 1024 + 2));
  await assert.rejects(next.turn.commit(), /startup buffer/); assert.equal(next.socket.closed, true);
});
test('live STT timeout and provider errors are actionable and never expose provider payload', async () => {
  const { turn, socket } = await live({ timeoutMs: 10 }); turn.append(tone(150)); socket.event({ type: 'session.updated' });
  await assert.rejects(turn.commit(), /timed out/); assert.equal(socket.closed, true);
  const next = await live(); next.turn.append(tone(150));
  next.socket.event({ type: 'error', error: { message: 'sk-secret' } });
  await assert.rejects(next.turn.commit(), (error) => error.code === 'stt_failed' && !error.message.includes('secret'));
});
test('engine live transcription starts only on beginTurn, cancels prior turns, and never calls batch STT', async () => {
  let batchCalls = 0;
  const engine = new OpenClawEngine({ liveTranscription: true, language: 'ru' }, {
    cfg: {}, core: { mediaUnderstanding: { transcribeAudioFile: () => { batchCalls++; } } },
    liveOptions: { Socket, credential: async () => 'test-key' },
  });
  const before = Socket.last;
  await tick(); assert.equal(Socket.last, before, 'constructing an idle engine opens no transcription socket');
  await assert.rejects(engine.commit(), /no active PTT/); assert.equal(batchCalls, 0);
  engine.beginTurn(); await tick(); const first = Socket.last; first.emit('open');
  engine.beginTurn(); await tick(); assert.equal(first.closed, true);
  const current = Socket.last; current.emit('open'); current.event({ type: 'session.updated' });
  engine.append(tone(200)); const pending = engine.commit();
  current.event({ type: 'input_audio_buffer.committed', item_id: 'x' });
  current.event({ type: 'conversation.item.input_audio_transcription.completed', item_id: 'x', transcript: 'same-agent input' });
  assert.equal(await pending, 'same-agent input'); assert.equal(batchCalls, 0);
  engine.close();
});
