import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { HttpEngine } from '../src/engines/http.js';
import { normalizeHostConfig, writeHostConfig, readHostConfig, VOICE_SETUP_FIELDS } from '../src/agent-sdk/index.js';
import { collectSetupValues } from '../src/agent-sdk/cli.js';
function directory(t) {
  const path = mkdtempSync(join(tmpdir(), 'kubik-agent-config-'));
  t.after(() => rmSync(path, { recursive: true, force: true }));
  return path;
}

test('host voice wizard persists every speech choice and migrates old state once', async (t) => {
  const stateDir = directory(t);
  const answers = ['https://speech.example/v1', 'custom-stt', 'custom-tts', 'custom-voice', 'en'];
  const voice = await collectSetupValues(VOICE_SETUP_FIELDS, async () => answers.shift());
  const adapter = { id: 'custom-fixture', setup: { apiKeyEnv: 'API_SERVER_KEY', model: 'fixture-agent' } };
  const config = { version: 2, listener: { port: 19000, host: '192.168.1.5' }, voice, adapter };
  await writeHostConfig(stateDir, config);
  assert.deepEqual(await readHostConfig(stateDir), config);
  assert.deepEqual(await collectSetupValues(VOICE_SETUP_FIELDS, async () => '', voice), voice);
  const path = join(stateDir, 'agent-host.json');
  const legacy = { version: 1, listener: config.listener, voiceBaseUrl: voice.baseUrl, adapter };
  writeFileSync(path, JSON.stringify(legacy));
  const migrated = await readHostConfig(stateDir);
  assert.equal(migrated.version, 2);
  assert.deepEqual(migrated.listener, config.listener);
  assert.deepEqual(migrated.adapter, adapter);
  assert.equal(migrated.voice.baseUrl, voice.baseUrl);
  assert.deepEqual(migrated.voice, await collectSetupValues(VOICE_SETUP_FIELDS, async () => '', { baseUrl: voice.baseUrl }));
  assert.equal(statSync(path).mode & 0o777, 0o600);
  const saved = readFileSync(path, 'utf8');
  assert.deepEqual(await readHostConfig(stateDir), migrated);
  assert.equal(readFileSync(path, 'utf8'), saved);
  for (const bad of [{ ...legacy, secret: 'must-not-save' }, { ...legacy, voiceBaseUrl: '' }, { ...legacy, version: 3 }]) {
    const original = JSON.stringify(bad);
    writeFileSync(path, original);
    await assert.rejects(readHostConfig(stateDir));
    assert.equal(readFileSync(path, 'utf8'), original);
  }
  for (const badVoice of [{ ...voice, apiKey: 'secret' }, { ...voice, voice: 'a\nsecret' }, { ...voice, language: 'English' }]) {
    assert.throws(() => normalizeHostConfig({ ...config, voice: badVoice }));
  }
});


test('wizard speech choices reach compatible HTTP requests without provider calls', async () => {
  const answers = ['https://speech.example/v1', 'custom-stt', 'custom-tts', 'custom-voice', 'en'];
  const voice = await collectSetupValues(VOICE_SETUP_FIELDS, async () => answers.shift());
  const requests = [];
  const engine = new HttpEngine({ ...voice, apiKey: 'fake-key' }, { fetchImpl: async (url, options) => {
    requests.push({ url, body: options.body });
    return url.endsWith('transcriptions') ? Response.json({ text: 'Hello' }) : new Response(new Uint8Array([0, 0]));
  } });
  engine.beginTurn(); engine.append(Buffer.alloc(4800));
  assert.equal(await engine.commit(), 'Hello');
  await engine.speak('Hello', { onAudio() {} });
  assert.equal(requests[0].url, `${voice.baseUrl}/audio/transcriptions`);
  assert.equal(requests[0].body.get('model'), voice.transcribeModel);
  assert.equal(requests[0].body.get('language'), voice.language);
  assert.equal(requests[1].url, `${voice.baseUrl}/audio/speech`);
  const body = JSON.parse(requests[1].body);
  assert.equal(body.model, voice.ttsModel); assert.equal(body.voice, voice.voice);
});
