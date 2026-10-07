import assert from 'node:assert/strict';
import test from 'node:test';
import { HttpEngine, OpenClawEngine, createEngine } from '../src/engines/index.js';
import { pcmRms } from './audio-analysis.js';
import { mockOpenAI, tone } from './helpers.js';

const voiceFor = (url, extra = {}) => {
  const voice = { provider: 'openai-http', baseUrl: url, voice: 'marin', transcribeModel: 'gpt-4o-transcribe',
    language: 'ru', ttsModel: 'gpt-4o-mini-tts', ...extra };
  Object.defineProperty(voice, 'apiKey', { value: 'sk-test-DO-NOT-LOG', enumerable: false });
  return voice;
};

async function speakAll(engine, text, options = {}) {
  const chunks = [];
  const result = await engine.speak(text, { onAudio: (pcm) => chunks.push(pcm), ...options });
  return { result, pcm: Buffer.concat(chunks) };
}

test('createEngine picks by provider', () => {
  assert.ok(createEngine(voiceFor('http://x/v1')) instanceof HttpEngine);
  const core = { mediaUnderstanding: { transcribeAudioFile() {} } };
  assert.ok(createEngine(voiceFor('http://x/v1', { provider: 'openclaw' }), { core }) instanceof OpenClawEngine);
});

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

test('late stale voice capability refresh cannot overwrite a newer committed mode', async () => {
  const voices = [], nativeReady = deferred();
  let nativeEntered;
  const entered = new Promise(resolve => { nativeEntered = resolve; });
  const sdk = {
    getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1'], resolveConfig: () => ({ model: 'gpt-realtime-2.1' }),
      isConfigured: () => { nativeEntered(); return nativeReady.promise; } }),
    resolveRealtimeVoiceProviderCapabilities: () => ({}),
  };
  const engine = new OpenClawEngine({ provider: 'openclaw', mode: 'classic' }, { core: {}, cfg: {}, nativeOptions: { sdk } });
  engine.getVoice = () => { const voice = deferred(); voices.push(voice); return voice.promise; };
  const stale = engine.refreshCapabilities({ agentId: 'stale' });
  const current = engine.refreshCapabilities({ agentId: 'current' });
  voices[0].resolve({ provider: 'openclaw', mode: 'realtime' });
  await entered;
  voices[1].resolve({ provider: 'openclaw', mode: 'classic' });
  await current;
  assert.equal(engine.voiceMode, 'classic');
  nativeReady.resolve(true);
  await stale;
  assert.equal(engine.voiceMode, 'classic');
  assert.equal(engine.agentId, 'current');
  assert.equal(engine.nativeVoice, null);
  assert.equal(engine.canListen, false);
});

test('failed voice capability refresh leaves the last committed voice state intact', async () => {
  const engine = new OpenClawEngine({ provider: 'openclaw', mode: 'classic' }, { core: {}, cfg: {}, nativeOptions: { sdk: {
    getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1'], resolveConfig: () => ({ model: 'gpt-realtime-2.1' }),
      async isConfigured() { throw Error('auth lookup failed'); } }),
    resolveRealtimeVoiceProviderCapabilities: () => ({}),
  } } });
  await engine.refreshCapabilities({ agentId: 'current' });
  const voice = engine.voice, nativeVoice = engine.nativeVoice;
  engine.getVoice = async () => ({ provider: 'openclaw', mode: 'realtime' });
  await assert.rejects(engine.refreshCapabilities({ agentId: 'failed' }), /auth lookup failed/);
  assert.equal(engine.voice, voice);
  assert.equal(engine.voiceMode, 'classic');
  assert.equal(engine.agentId, 'current');
  assert.equal(engine.nativeVoice, nativeVoice);
  assert.equal(engine.canListen, false);
});

test('an older successful capability refresh still commits after a newer refresh fails', async () => {
  const voices = [];
  const sdk = {
    getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1'], resolveConfig: () => ({ model: 'gpt-realtime-2.1' }),
      async isConfigured() { throw Error('auth lookup failed'); } }),
    resolveRealtimeVoiceProviderCapabilities: () => ({}),
  };
  const engine = new OpenClawEngine({ provider: 'openclaw', mode: 'classic' }, { core: {}, cfg: {}, nativeOptions: { sdk } });
  await engine.refreshCapabilities({ agentId: 'initial' });
  engine.getVoice = () => { const voice = deferred(); voices.push(voice); return voice.promise; };
  const older = engine.refreshCapabilities({ agentId: 'older' });
  const newer = engine.refreshCapabilities({ agentId: 'newer' });
  voices[1].resolve({ provider: 'openclaw', mode: 'realtime' });
  await assert.rejects(newer, /auth lookup failed/);
  voices[0].resolve({ provider: 'openclaw', mode: 'classic' });
  await older;
  assert.equal(engine.agentId, 'older');
  assert.equal(engine.voiceMode, 'classic');
  assert.equal(engine.nativeVoice, null);
});

test('http engine: WAV transcription and streamed pcm speech (mock)', async (t) => {
  const mock = await mockOpenAI(t);
  const engine = new HttpEngine(voiceFor(mock.url));
  engine.beginTurn();
  engine.append(tone(1000));
  assert.equal(await engine.commit(), 'Привет! Как дела?');
  engine.beginTurn();
  engine.append(Buffer.alloc(48 * 800));
  assert.equal(await engine.commit(), '', 'silence');
  const { result, pcm } = await speakAll(engine, 'Проверка связи.');
  assert.equal(result.cancelled, false);
  assert.ok(pcm.length > 24000 && pcm.length % 2 === 0);
  assert.ok(pcmRms(pcm) > 0.05);
});

test('http engine: cancel and HTTP errors', async (t) => {
  const mock = await mockOpenAI(t, { audioChunkDelayMs: 20 });
  const engine = new HttpEngine(voiceFor(mock.url));
  let bytes = 0;
  const pending = engine.speak('Длинная фраза для проверки отмены потоковой речи по HTTP.', { onAudio: (pcm) => { bytes += pcm.length; engine.cancel(); } });
  assert.deepEqual(await pending, { cancelled: true });
  assert.ok(bytes > 0);
  const broken = new HttpEngine(voiceFor(`${mock.url}/missing`));
  await assert.rejects(broken.speak('x'), /HTTP 404/);
  broken.beginTurn();
  broken.append(tone(500));
  await assert.rejects(broken.commit(), (error) => error.code === 'stt_failed');
});


test('HTTP STT bounds stated and chunked responses and transcript length', async () => {
  const cases = [
    [() => new Response(JSON.stringify({ text: ' Normal transcript ' })), 'Normal transcript'],
    [() => new Response('{}', { headers: { 'content-length': '65537' } }), null],
    [() => new Response(new ReadableStream({ start(c) {
      c.enqueue(new Uint8Array(32768)); c.enqueue(new Uint8Array(32769)); c.close();
    } })), null],
    [() => new Response(JSON.stringify({ text: 'x'.repeat(8193) })), null],
  ];
  for (const [response, expected] of cases) {
    const engine = new HttpEngine(voiceFor('https://stt.example/v1'), { fetchImpl: async () => response() });
    engine.beginTurn(); engine.append(tone(500));
    if (expected === null) await assert.rejects(engine.commit(), (error) => error.code === 'stt_failed');
    else assert.equal(await engine.commit(), expected);
  }
});

test('HTTP STT cancellation and timeout abort a body still reading after headers', async () => {
  for (const cancel of [true, false]) {
    let readStarted, requestSignal;
    const reading = new Promise((resolve) => { readStarted = resolve; });
    const engine = new HttpEngine(voiceFor('https://stt.example/v1'), {
      transcribeTimeoutMs: cancel ? 1000 : 30,
      fetchImpl: async (_url, { signal }) => {
        requestSignal = signal;
        return new Response(new ReadableStream({
          start(controller) { signal.addEventListener('abort', () => controller.error(signal.reason), { once: true }); },
          pull() { readStarted(); },
        }));
      },
    });
    engine.beginTurn(); engine.append(tone(500));
    const pending = engine.commit();
    const rejected = assert.rejects(pending, cancel ? { name: 'AbortError' } : (e) => e.code === 'stt_failed');
    await reading;
    if (cancel) engine.cancelTranscription();
    await rejected;
    assert.equal(requestSignal.aborted, true);
    engine.beginTurn(); engine.close();
  }
});
