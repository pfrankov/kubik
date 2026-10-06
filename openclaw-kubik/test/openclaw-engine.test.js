import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { chmod, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { mulawToPcm16, parseWav, pcmToWav, resamplePcm16 } from '../src/audio.js';
import { createEngine, OpenClawEngine } from '../src/engines/index.js';
import { kubikTtsConfig, resolveOpenClawVoiceCapabilities, toDevicePcm } from '../src/engines/openclaw.js';
import { resolveAccount } from '../src/config.js';
import { account, config, fakeCore, tone } from './helpers.js';
import { fakeSpeechSdk, speechConfig } from './fake-speech-sdk.js';

async function speechEngine(voice, { core, cfg = {}, ...options }) {
  const engine = createEngine(voice, { core, cfg: speechConfig(cfg), speechSdk: fakeSpeechSdk(core), ...options });
  await engine.refreshCapabilities();
  return engine;
}

const openclawVoice = () => account({ voice: { provider: 'openclaw' } }).voice;

test('audio conversion: resample, mu-law, WAV parsing', () => {
  const pcm16k = Buffer.alloc(16000 * 2); // 1 s
  for (let i = 0; i < 16000; i++) pcm16k.writeInt16LE(Math.round(Math.sin(i / 10) * 10000), i * 2);
  const up = resamplePcm16(pcm16k, 16000, 24000);
  assert.equal(up.length, 24000 * 2);
  assert.equal(resamplePcm16(pcm16k, 24000, 24000).length, pcm16k.length);
  assert.throws(() => resamplePcm16(pcm16k, 0), RangeError);
  const mulaw = mulawToPcm16(Buffer.from([0xff, 0x7f, 0x00, 0x80]));
  assert.deepEqual([0, 1, 2, 3].map((i) => mulaw.readInt16LE(i * 2)), [0, 0, -32124, 32124]);
  const wav = parseWav(pcmToWav(Buffer.alloc(480), 16000));
  assert.equal(wav.sampleRate, 16000);
  assert.equal(wav.pcm.length, 480);
  assert.equal(parseWav(Buffer.from('not a wav')), null);
});

test('toDevicePcm normalises telephony results to 24 kHz s16le', () => {
  assert.equal(toDevicePcm({ audioBuffer: Buffer.alloc(4801), outputFormat: 'pcm', sampleRate: 24000 }).length, 4800);
  assert.equal(toDevicePcm({ audioBuffer: Buffer.alloc(16000 * 2), outputFormat: 'pcm', sampleRate: 16000 }).length, 48000);
  assert.equal(toDevicePcm({ audioBuffer: Buffer.alloc(22050 * 2), outputFormat: 'pcm_22050' }).length, 48000);
  assert.equal(toDevicePcm({ audioBuffer: Buffer.alloc(8000), outputFormat: 'ulaw_8000', sampleRate: 8000 }).length, 48000);
  assert.equal(toDevicePcm({ audioBuffer: pcmToWav(Buffer.alloc(32000), 16000), outputFormat: 'wav' }).length, 48000);
  assert.throws(() => toDevicePcm({ audioBuffer: Buffer.alloc(10), outputFormat: 'mp3' }), /unsupported/);
});

test('kubik tts override layers over the global tts block', () => {
  const cfg = { tts: { auto: 'tagged', provider: 'openai', providers: { openai: { voice: 'alloy', model: 'm' } } },
    channels: { kubik: { tts: { providers: { openai: { voice: 'shimmer' } } } } } };
  assert.deepEqual(kubikTtsConfig(cfg).tts, { auto: 'tagged', provider: 'openai', providers: { openai: { voice: 'shimmer', model: 'm' } } });
  assert.equal(kubikTtsConfig({ tts: { a: 1 } }).tts.a, 1);
});

test('voice capability detection uses configured STT candidates and one PCM-capable TTS provider', async () => {
  const { core, calls } = fakeCore();
  const cfg = { tools: { media: { audio: { models: [{ provider: 'groq', model: 'legacy-ignored' }] },
    models: [{ provider: 'openai', model: 'chosen-stt', capabilities: ['audio'] }] } },
  tts: { provider: 'openai', providers: { openai: { apiKey: 'test-key' } } } };
  const available = await resolveOpenClawVoiceCapabilities({ core, cfg });
  assert.deepEqual(available.stt, { available: true, provider: 'openai', model: 'chosen-stt' });
  assert.deepEqual(available.tts, { available: true, provider: 'openai', model: 'gpt-4o-mini-tts' });
  assert.equal(calls.stt.length, 0, 'capability detection does not transcribe or bill');
  assert.equal(calls.tts.length, 0, 'capability detection does not synthesize or bill');

  const unsupported = await resolveOpenClawVoiceCapabilities({ core,
    cfg: { tts: { provider: 'microsoft', providers: { microsoft: { apiKey: 'test-key' } } } } });
  assert.deepEqual(unsupported.tts, { available: false }, 'unsupported output is not advertised with a fallback provider');
});

test('native capability refresh skips classic STT discovery and retains notification TTS', async () => {
  const { core, calls } = fakeCore();
  core.modelAuth.resolveApiKeyForProvider = async () => { throw Error('Unexpected classic STT probe'); };
  const cfg = { tts: { provider: 'openai', providers: { openai: { apiKey: 'test-key' } } } };
  for (const mode of ['realtime', 'live']) for (const available of [true, false]) {
    let nativeChecks = 0, classicChecks = 0;
    core.mediaUnderstanding = { get transcribeAudioFile() { classicChecks++; return () => {}; } };
    const provider = { resolveConfig: ({ rawConfig }) => rawConfig, isConfigured: () => { nativeChecks++; return available; } };
    const engine = new OpenClawEngine({ ...openclawVoice(), mode }, { core, cfg, nativeOptions: { sdk: {
      getRealtimeVoiceProvider: () => provider, resolveRealtimeVoiceProviderCapabilities: () => ({}),
    } } });
    const caps = await engine.refreshCapabilities({ agentId: 'device-agent' });
    assert.equal(nativeChecks, 1); assert.equal(classicChecks, 0);
    assert.equal(caps.stt.available, available); assert.equal(engine.canListen, available);
    assert.equal(engine.canSpeak, true); assert.equal(caps.tts.provider, 'openai');
  }
  assert.equal(calls.stt.length, 0); assert.equal(calls.tts.length, 0);
});

test('live STT capability requires an OpenAI API-key, and local CLI detection checks actual executables', async (t) => {
  const { core } = fakeCore();
  core.modelAuth.resolveApiKeyForProvider = async () => ({ mode: 'oauth', apiKey: 'oauth-token' });
  const liveVoice = { ...openclawVoice(), liveTranscription: true };
  const oauth = await resolveOpenClawVoiceCapabilities({ core, cfg: {}, voice: liveVoice, env: {} });
  assert.deepEqual(oauth.stt, { available: false });
  core.modelAuth.resolveApiKeyForProvider = async () => ({ mode: 'api-key', apiKey: 'test-key' });
  const apiKey = await resolveOpenClawVoiceCapabilities({ core, cfg: {}, voice: liveVoice, env: {} });
  assert.deepEqual(apiKey.stt, { available: true, provider: 'openai', model: 'gpt-live-transcribe' });

  const dir = await mkdtemp(join(tmpdir(), 'kubik-audio-cli-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const whisper = join(dir, 'whisper');
  await writeFile(whisper, '#!/bin/sh\nexit 0\n');
  await chmod(whisper, 0o755);
  core.modelAuth.resolveApiKeyForProvider = async () => null;
  const local = await resolveOpenClawVoiceCapabilities({ core, cfg: {}, env: { PATH: dir, HOME: dir }, platform: 'linux', arch: 'x64' });
  assert.deepEqual(local.stt, { available: true, provider: 'local', model: 'whisper' });
});

test('config: provider openclaw needs no key or URL and ignores OPENAI_API_KEY', () => {
  const acc = resolveAccount(config({ voice: { provider: 'openclaw' }, allowInsecureBaseUrl: false, tts: { auto: 'off' } }), 'default',
    { env: { OPENAI_API_KEY: 'sk-env-DO-NOT-USE' } });
  assert.equal(acc.voice.provider, 'openclaw');
  assert.equal(acc.voice.apiKey, '');
  assert.equal(acc.voice.apiKeySource, 'openclaw');
  assert.throws(() => resolveAccount(config({ voice: { provider: 'openclaw', apiKey: 'sk-x' } }), 'default', { env: {} }), /remove voice\.apiKey/);
  assert.equal(createEngine(acc.voice, {}).canListen, false);
});

test('HTTP voice capability requires its configured key and model ids', () => {
  assert.equal(createEngine(account().voice).canListen, true);
  assert.equal(createEngine(account().voice).canSpeak, true);
  const noKey = resolveAccount({ channels: { kubik: { voice: { provider: 'openai-http', baseUrl: 'https://api.example/v1' } } } },
    'default', { env: {} }).voice;
  assert.equal(createEngine(noKey).canListen, false);
  assert.equal(createEngine(noKey).canSpeak, false);
});

test('openclaw engine: STT gets a 24 kHz WAV and the configured language; temp file is removed', async () => {
  const { core, calls } = fakeCore();
  const cfg = { marker: 1 };
  const engine = await speechEngine(openclawVoice(), { core, cfg });
  assert.ok(engine instanceof OpenClawEngine);
  engine.beginTurn();
  engine.append(tone(800));
  assert.equal(await engine.commit(), 'Привет, Кубик');
  assert.equal(calls.stt.length, 1);
  assert.equal(calls.stt[0].sampleRate, 24000);
  assert.equal(calls.stt[0].bytes, 48 * 800);
  assert.equal(calls.stt[0].language, 'ru');
  assert.equal(calls.stt[0].mime, 'audio/wav');
  assert.equal(calls.stt[0].cfg.marker, cfg.marker);
  assert.equal(existsSync(calls.stt[0].filePath), false);
  engine.append(tone(50));
  assert.equal(await engine.commit(), '', 'too short: no STT call');
  assert.equal(calls.stt.length, 1);
});

test('openclaw engine: STT failures map to stt_failed; skipped means empty', async () => {
  const failed = createEngine(openclawVoice(), { core: fakeCore({ transcript: '', outcome: 'failed' }).core, cfg: {} });
  failed.append(tone(500));
  await assert.rejects(failed.commit(), (error) => error.code === 'stt_failed');
  const skipped = createEngine(openclawVoice(), { core: fakeCore({ transcript: '', outcome: 'skipped' }).core, cfg: {} });
  skipped.append(tone(500));
  assert.equal(await skipped.commit(), '');
  const { core } = fakeCore();
  core.mediaUnderstanding.transcribeAudioFile = async () => { throw new Error('provider says 401 sk-SECRET'); };
  const thrown = createEngine(openclawVoice(), { core, cfg: {} });
  thrown.append(tone(500));
  await assert.rejects(thrown.commit(), (error) => error.code === 'stt_failed' && !error.message.includes('SECRET'));
  const silent = fakeCore();
  silent.core.mediaUnderstanding.transcribeAudioFile = async () => { throw new Error('Audio transcription response missing text'); };
  const quiet = createEngine(openclawVoice(), { core: silent.core, cfg: {} });
  quiet.append(tone(500));
  assert.equal(await quiet.commit(), '');
});

test('openclaw engine: cancelTranscription rejects at once', async () => {
  const { core } = fakeCore();
  let release;
  core.holdStt = new Promise((r) => { release = r; });
  const engine = createEngine(openclawVoice(), { core, cfg: {} });
  engine.append(tone(500));
  const pending = engine.commit();
  setTimeout(() => engine.cancelTranscription(), 20);
  await assert.rejects(pending, { name: 'AbortError' });
  release();
});

test('openclaw engine: speech is resampled to 24 kHz and uses the kubik tts override', async () => {
  const { core, calls } = fakeCore({ telephony: () => ({ success: true, provider: 'x', audioBuffer: Buffer.alloc(16000 * 2 * 3), outputFormat: 'pcm', sampleRate: 16000 }) });
  const cfg = { tts: { provider: 'openai' }, channels: { kubik: { tts: { provider: 'elevenlabs' } } } };
  const engine = await speechEngine(openclawVoice(), { core, cfg });
  let bytes = 0;
  const chunks = [];
  const result = await engine.speak('Привет!', { onAudio: (pcm) => { bytes += pcm.length; chunks.push(pcm.length); } });
  assert.deepEqual(result, { cancelled: false });
  assert.equal(bytes, 48 * 3000);
  assert.ok(chunks.every((n) => n % 2 === 0));
  assert.equal(calls.tts[0].text, 'Привет!');
  assert.equal(calls.tts[0].cfg.tts.provider, 'elevenlabs');
});

test('openclaw engine: speech failure is a VoiceError; cancel drops late audio', async () => {
  const bad = await speechEngine(openclawVoice(), { core: fakeCore({ telephony: () => ({ success: false, error: 'boom sk-SECRET' }) }).core, cfg: {} });
  await assert.rejects(bad.speak('x', { onAudio: () => {} }), (error) => error.name === 'VoiceError' && !error.message.includes('SECRET'));
  const { core } = fakeCore();
  let release;
  core.holdTts = new Promise((r) => { release = r; });
  const engine = await speechEngine(openclawVoice(), { core, cfg: {} });
  let audio = 0;
  const pending = engine.speak('Долгий ответ', { onAudio: (pcm) => { audio += pcm.length; } });
  setTimeout(() => engine.cancel(), 20);
  assert.deepEqual(await pending, { cancelled: true });
  release();
  await new Promise((r) => setTimeout(r, 20));
  assert.equal(audio, 0);
});


test('openclaw engine: voice.pitch shifts the voice up and keeps its duration', async () => {
  // 3 s of a 150 Hz tone at 24 kHz.
  const tonePcm = Buffer.alloc(24000 * 2 * 3);
  for (let i = 0; i < 24000 * 3; i++) tonePcm.writeInt16LE(Math.round(8000 * Math.sin((2 * Math.PI * 150 * i) / 24000)), i * 2);
  const { core } = fakeCore({ telephony: () => ({ success: true, provider: 'x', audioBuffer: tonePcm, outputFormat: 'pcm', sampleRate: 24000 }) });
  const voice = account({ voice: { provider: 'openclaw', pitch: 6 } }).voice;
  assert.equal(voice.pitch, 6);
  const engine = await speechEngine(voice, { core, cfg: {} });
  const parts = [];
  await engine.speak('Привет!', { onAudio: (pcm) => parts.push(pcm) });
  const out = Buffer.concat(parts);
  assert.equal(out.length, tonePcm.length);
  // Count zero crossings in the middle second: 150 Hz * 2^(6/12) ≈ 212 Hz.
  let crossings = 0;
  for (let i = 24000 + 1; i < 48000; i++) if ((out.readInt16LE(i * 2 - 2) < 0) !== (out.readInt16LE(i * 2) < 0)) crossings++;
  assert.ok(Math.abs(crossings / 2 - 212) < 8, `got ${crossings / 2} Hz`);
});
