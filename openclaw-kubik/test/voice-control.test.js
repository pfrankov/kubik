import test, { after } from 'node:test';
import { mkdtempSync, rmSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createVoiceModelStore } from '../src/voice-model-store.js';
const paths = [];
after(() => paths.forEach(path => rmSync(path, { recursive: true, force: true })));
function temporary() { const path = mkdtempSync(join(tmpdir(), 'kubik-voice-')); paths.push(path); return path; }
import assert from 'node:assert/strict';
import { createVoiceControl } from '../src/voice-control.js';
import { selectedSttConfig } from '../src/engines/openclaw.js';
import { SpeechStream } from '../src/speech-stream.js';

function fixture(mode = 'realtime') {
  const directory = temporary();
  const core = { state: { resolveStateDir: () => directory } };
  const account = { accountId: 'main', voice: { provider: 'openclaw', mode } };
  const runtime = {
    resolveOpenClawVoiceCapabilities: async () => ({ stt: { available: true, provider: 'local', model: 'stt' }, tts: { available: false } }),
    getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1', 'gpt-realtime-2.1-mini', 'gpt-live-1'] }),
    resolveNativeVoice: async ({ voice }) => {
      if (voice.mode === 'classic') return null;
      const metadata = { available: true, provider: 'openai', model: voice.model ?? (voice.mode === 'live' ? 'gpt-live-1' : 'gpt-realtime-2.1') };
      return { available: true, stt: metadata, tts: metadata };
    },
  };
  const make = () => createVoiceControl({ core, cfg: {}, account, agentId: () => 'agent', runtime });
  return { make, account, runtime, path: join(directory, 'kubik/voice-models.json') };
}
const device = { id: 'cube', fingerprint: 'paired-key' };

test('native voice models persist per device and mode without changing the agent', async () => {
  const { make, account, path } = fixture();
  const control = make();
  assert.deepEqual((await control.options(device, 'voice')).models.map(row => row.id),
    ['openai/gpt-realtime-2.1', 'openai/gpt-realtime-2.1-mini']);
  await control.selectModel(device, 'voice', 'openai/gpt-realtime-2.1-mini');
  assert.equal((await make().options(device, 'voice')).model, 'openai/gpt-realtime-2.1-mini');
  assert.equal((await control.settings({ ...device, id: 'other' })).model, undefined);
  assert.equal((await control.settings({ ...device, fingerprint: 'new-key' })).model, undefined);
  assert.equal(account.voice.model, undefined);
  await assert.rejects(control.selectModel(device, 'voice', 'openai/gpt-live-1'), { code: 'invalid_model' });
  account.voice.mode = 'live';
  assert.equal((await control.settings(device)).model, undefined);
  assert.deepEqual((await control.options(device, 'voice')).models.map(row => row.id), ['openai/gpt-live-1']);
  assert.equal(JSON.parse(readFileSync(path)).entries.length, 1);
  assert.equal(statSync(path).mode & 0o777, 0o600);
});

test('modes switch locally, retain their own models, and survive controller recreation', async () => {
  const { make, account } = fixture(); const control = make();
  account.voice.model = 'gpt-realtime-2.1';
  assert.deepEqual((await control.options(device, 'mode')).models, [
    { id: 'classic', label: 'STT', available: true }, { id: 'realtime', label: 'Realtime', available: true },
    { id: 'live', label: 'GPT Live', available: true },
  ]);
  await control.selectModel(device, 'voice', 'openai/gpt-realtime-2.1-mini');
  await control.selectModel(device, 'mode', 'live');
  assert.equal((await make().settings(device)).mode, 'live');
  assert.equal((await make().options(device, 'voice')).model, 'openai/gpt-live-1');
  await control.selectModel(device, 'voice', 'openai/gpt-live-1');
  await assert.rejects(control.options(device, 'stt'), { code: 'unsupported' });
  await control.selectModel(device, 'mode', 'classic');
  assert.equal((await control.settings(device)).mode, 'classic');
  await assert.rejects(control.options(device, 'voice'), { code: 'unsupported' });
  assert.equal((await control.options(device, 'stt')).stt.available, true);
  await control.selectModel(device, 'mode', 'realtime');
  assert.equal((await make().options(device, 'voice')).model, 'openai/gpt-realtime-2.1-mini');
  assert.equal((await control.settings({ ...device, fingerprint: 'another' })).mode, 'realtime');
  assert.equal(account.voice.mode, 'realtime'); assert.equal(account.voice.model, 'gpt-realtime-2.1');
});

test('mode discovery does not hide STT when native setup is unavailable and rejects a disabled selection', async () => {
  const { make, runtime } = fixture('classic');
  runtime.getRealtimeVoiceProvider = () => null;
  const control = make();
  const data = await control.options(device, 'mode');
  assert.deepEqual(data.models.map(row => row.available), [true, false, false]);
  await assert.rejects(control.selectModel(device, 'mode', 'live'), { code: 'unavailable' });
  await assert.rejects(control.selectModel(device, 'mode', 'invented'), { code: 'invalid_model' });
  assert.equal((await control.settings(device)).mode, 'classic');
});

test('a persisted native mode remains visible when its provider is removed', async () => {
  const { make, runtime } = fixture('realtime'); const control = make();
  await control.selectModel(device, 'mode', 'live');
  runtime.getRealtimeVoiceProvider = () => undefined;
  const reloaded = make();
  const current = await reloaded.options(device, 'mode');
  assert.equal(current.model, 'live'); assert.equal(current.stt.available, false);
  assert.deepEqual(current.models.map(row => row.available), [true, false, false]);
  assert.deepEqual((await reloaded.options(device, 'voice')).models, []);
  await reloaded.selectModel(device, 'mode', 'classic');
  assert.equal((await control.settings(device)).mode, 'classic');
});

test('explicit input selection preserves secret-owner indices and has no second audio candidate', () => {
  const rows = [ { provider: 'a', model: 'one', capabilities: ['audio', 'image'] },
    { provider: 'b', model: 'two', capabilities: ['audio'], apiKey: { source: 'env', id: 'TEST' } } ];
  const cfg = { tools: { media: { models: rows, audio: { enabled: true } } } };
  const selected = selectedSttConfig(cfg, { provider: 'b', model: 'two' });
  assert.equal(selected.tools.media.models.length, rows.length);
  assert.equal(selected.tools.media.models[1], rows[1]);
  assert.deepEqual(selected.tools.media.models[0], {});
  assert.deepEqual(cfg.tools.media.models[0].capabilities, ['audio', 'image']);
  assert.throws(() => selectedSttConfig(cfg, { provider: 'x', model: 'missing' }), /unavailable/);
});

test('native PCM delta granularity does not exhaust the bounded queue or alter samples', () => {
  for (const size of [96, 4800, 96000]) {
    const stream = new SpeechStream('reply', 1);
    const input = Buffer.alloc(8000 * 48);
    for (let i = 0; i < input.length; i++) input[i] = i % 251;
    for (let at = 0; at < input.length; at += size) stream.enqueuePcm(input.subarray(at, at + size), 400 * 48);
    assert.equal(stream.items.length, 20);
    assert.deepEqual(Buffer.concat(stream.items.map(item => item.pcm)), input);
  }
});

 test('voice preferences serialize concurrent writes and reject corrupt or oversized state', async () => {
  const directory = temporary(), store = createVoiceModelStore(directory);
  await Promise.all([store.select('device', 'stt', 'openai/one'), createVoiceModelStore(directory).select('device', 'tts', 'openai/two')]);
  assert.deepEqual(store.lookup('device'), { stt: 'openai/one', tts: 'openai/two' });
  const path = join(directory, 'voice-models.json');
  writeFileSync(path, JSON.stringify({ version: 1, entries: [['device', { unknown: 'openai/one' }]] }));
  assert.throws(() => store.lookup('device'), /Invalid/);
  await assert.rejects(store.select('device', 'tts', 'openai/two'), /Invalid/);
  for (const entries of [[['device', true]], [['device', 5]], [['device', {}, 'extra']], [{ device: {} }]]) {
    writeFileSync(path, JSON.stringify({ version: 1, entries }));
    assert.throws(() => store.lookup('device'), /Invalid/);
  }
  writeFileSync(path, 'x'.repeat(128 * 1024 + 1));
  assert.throws(() => store.lookup('device'), /too large/);
  writeFileSync(path, JSON.stringify({ version: 1, entries: Array.from({ length: 128 }, (_, n) => [String(n), { stt: 'openai/one' }]) }));
  await assert.rejects(store.select('new', 'tts', 'openai/two'), /full/);
  await store.select('0', 'tts', 'openai/two');
  assert.equal(store.lookup('0').tts, 'openai/two');
});
