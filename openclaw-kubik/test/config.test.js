import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { channelSchema, inspectAccount, normalizeBaseUrl, resolveAccount, uiHints } from '../src/config.js';
import { channelPlugin } from '../src/channel.js';
import { setupPlugin } from '../src/channel-setup.js';
import { account, config, DEVICE } from './helpers.js';
import { validateJsonSchemaValue } from 'openclaw/plugin-sdk/json-schema-runtime';

const manifest = JSON.parse(readFileSync(new URL('../openclaw.plugin.json', import.meta.url), 'utf8'));
const pkg = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));

test('Gateway schema rejects incompatible native settings before saving or starting the channel', () => {
  const accepts = (voice) => validateJsonSchemaValue({ schema: channelSchema, value: { voice } }).ok;
  for (const mode of ['realtime', 'live']) {
    assert.equal(accepts({ mode, liveTranscription: true }), false);
    assert.equal(accepts({ mode, provider: 'openai-http' }), false);
    assert.equal(accepts({ mode, provider: 'openclaw', liveTranscription: false }), true);
    assert.equal(accepts({ mode }), true);
  }
  assert.equal(accepts({ mode: 'classic', liveTranscription: true }), true);
  assert.equal(accepts({ provider: 'openai-http' }), true);
});

test('every schema object forbids unknown keys', () => {
  const walk = (schema, path) => {
    if (schema.type !== 'object') return;
    assert.ok(schema.additionalProperties === false || typeof schema.additionalProperties === 'object', path);
    for (const [key, child] of Object.entries(schema.properties ?? {})) walk(child, `${path}.${key}`);
    if (typeof schema.additionalProperties === 'object') walk(schema.additionalProperties, `${path}.*`);
  };
  walk(channelSchema, 'kubik');
});

test('defaults resolve as documented', () => {
  const resolved = resolveAccount({ channels: { kubik: { devices: { [DEVICE]: { name: 'Desk' } } } } }, undefined, { env: {} });
  assert.deepEqual(resolved.listen, { enabled: true, public: false, port: 18790 });
  assert.equal(resolved.voice.provider, 'openclaw');
  assert.equal(resolved.voice.baseUrl, 'https://api.openai.com/v1');
  assert.equal(resolved.voice.voice, 'marin');
  assert.equal(resolved.voice.transcribeModel, 'gpt-4o-transcribe');
  assert.equal(resolved.voice.language, 'ru');
  assert.equal(resolved.voice.ttsModel, 'gpt-4o-mini-tts');
  assert.equal(resolved.configured, true);
  assert.equal(resolved.devices.get(DEVICE).name, 'Desk');
});

test('http baseUrl requires allowInsecureBaseUrl; credentials in URL are rejected', () => {
  assert.throws(() => normalizeBaseUrl('http://127.0.0.1:18800/v1'), /cleartext/);
  assert.equal(normalizeBaseUrl('http://127.0.0.1:18800/v1/', true), 'http://127.0.0.1:18800/v1');
  assert.throws(() => normalizeBaseUrl('https://user:pw@api.openai.com/v1'), /without credentials/);
  assert.throws(() => normalizeBaseUrl('ftp://x'), /HTTP\(S\)/);
  assert.throws(() => resolveAccount(config({ allowInsecureBaseUrl: false }), 'default', { env: {} }), /cleartext/);
});

test('API key: config, file and environment; never enumerable', async (t) => {
  const dir = await mkdtemp(join(tmpdir(), 'kubik-test-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const keyFile = join(dir, 'key');
  await writeFile(keyFile, 'sk-file-SECRET\n');
  const fromConfig = account();
  assert.equal(fromConfig.voice.apiKey, 'sk-test-DO-NOT-LOG');
  assert.equal(fromConfig.voice.apiKeySource, 'config');
  assert.ok(!JSON.stringify(fromConfig.voice).includes('DO-NOT-LOG'));
  assert.ok(!('apiKey' in structuredClone(fromConfig.voice)));
  const fromFile = resolveAccount(config({ voice: { provider: 'openai-http', baseUrl: 'https://x/v1', apiKeyFile: keyFile } }), 'default', { env: {} });
  assert.equal(fromFile.voice.apiKey, 'sk-file-SECRET');
  const fromEnv = resolveAccount(config({ voice: { provider: 'openai-http', baseUrl: 'https://x/v1' } }), 'default', { env: { OPENAI_API_KEY: 'sk-env' } });
  assert.equal(fromEnv.voice.apiKey, 'sk-env');
  assert.equal(fromEnv.voice.apiKeySource, 'env');
  const noRead = resolveAccount(config({ voice: { provider: 'openai-http', baseUrl: 'https://x/v1' } }), 'default', { env: { OPENAI_API_KEY: 'sk-env' }, readSecrets: false });
  assert.equal(noRead.voice.apiKey, '');
});

test('single implicit account; inspect never reads the voice secret', async (t) => {
  assert.throws(() => resolveAccount(config(), 'other', { env: {} }), /single implicit/);
  const dir = await mkdtemp(join(tmpdir(), 'kubik-test-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const file = join(dir, 'key');
  await writeFile(file, 'sk-SECRET');
  const inspected = inspectAccount(config({ voice: { provider: 'openai-http', baseUrl: 'https://example.test/v1', apiKeyFile: file } }), 'default');
  assert.equal(inspected.configured, true);
  assert.equal(inspected.apiKeySource, 'file');
  assert.ok(!JSON.stringify(inspected).includes('SECRET'));
});

test('configured = channel section present and enabled; devices are optional (pairing)', () => {
  assert.equal(account({ devices: {} }).configured, true);
  assert.equal(resolveAccount({ channels: { kubik: {} } }, 'default', { env: {} }).configured, true);
  assert.equal(resolveAccount({ channels: { kubik: { enabled: false } } }, 'default', { env: {} }).configured, false);
  assert.equal(resolveAccount({}, 'default', { env: {} }).configured, false);
  const revoked = account({ devices: { [DEVICE]: { enabled: false }, 'kubik-2': { name: 'Кухня' } } });
  assert.equal(revoked.configured, true);
  assert.equal(revoked.devices.get(DEVICE).enabled, false);
  assert.equal(inspectAccount({ channels: { kubik: {} } }, 'default').devices, 0);
  const [issue] = channelPlugin.status.collectStatusIssues([{ accountId: 'default', configured: false }]);
  assert.match(issue.message, /openclaw channels add --channel kubik/);
});

test('setup adapter validates input and writes a device without promoting accounts', () => {
  const { setup } = setupPlugin;
  assert.equal(setup.configPromotion, 'preserve-root');
  assert.equal(setup.validateInput({ input: {} }), null, 'pairing uses device-held keys');
  assert.deepEqual(setup.applyAccountConfig({ cfg: {}, input: { enable: true } }).channels.kubik, { enabled: true });
  assert.match(setup.validateInput({ input: { deviceId: 'Bad Id!' } }), /Invalid device id/);
  assert.deepEqual(setup.applyAccountConfig({ cfg: {}, input: {} }).channels.kubik, { enabled: true });
  assert.deepEqual(setup.applyAccountConfig({ cfg: {}, input: { deviceId: DEVICE, deviceName: 'Стол' } }).channels.kubik,
    { enabled: true, devices: { [DEVICE]: { enabled: true, name: 'Стол' } }, defaultTo: `kubik:${DEVICE}` });
  assert.match(setup.validateInput({ accountId: 'x', input: { deviceId: DEVICE } }), /one account/);
  const cfg = setup.applyAccountConfig({ cfg: { channels: { kubik: { voice: { language: 'ru' } } } }, input: { deviceId: DEVICE.toUpperCase() } });
  assert.deepEqual(cfg.channels.kubik, { voice: { language: 'ru' }, enabled: true, devices: { [DEVICE]: { enabled: true } }, defaultTo: `kubik:${DEVICE}` });
  const named = setup.applyAccountConfig({ cfg, input: { deviceId: DEVICE, deviceName: 'Кубик' } });
  assert.deepEqual(named.channels.kubik.devices[DEVICE], { enabled: true, name: 'Кубик' });
  assert.equal(setupPlugin.setupContract.kind, 'channel-owned');
  const renamed = setup.applyAccountName({ cfg, name: 'Стол' });
  assert.equal(renamed.channels.kubik.name, 'Стол');
});

test('status and security surfaces never include secrets', () => {
  const acc = account();
  const snapshot = channelPlugin.status.buildAccountSnapshot({ account: acc, runtime: { running: true } });
  assert.ok(!JSON.stringify(snapshot).includes('DO-NOT-LOG'));
  assert.equal(snapshot.insecureVoiceUrl, true);
  const warnings = channelPlugin.security.collectWarnings({ account: acc });
  assert.ok(warnings.some((w) => /allowInsecureBaseUrl/.test(w)));
  assert.ok(!JSON.stringify(warnings).includes('DO-NOT-LOG'));
});

test('listen.public opts the TLS listener into public peers', () => {
  assert.equal(resolveAccount(config({ listen: { public: true } }), 'default', { env: {} }).listen.public, true);
});

test('listen.enabled false turns the LAN listener off', () => {
  const acc = resolveAccount(config({ listen: { enabled: false } }), 'default', { env: {} });
  assert.deepEqual(acc.listen, { enabled: false, public: false, port: 18790 });
});

test('agent prompt hints describe spoken, tagged replies', () => {
  const hints = channelPlugin.agentPrompt.inboundFormattingHints({});
  assert.equal(hints.text_markup, 'plain_speech');
  const rules = hints.rules.join('\n');
  assert.match(rules, /Russian/);
  assert.match(rules, /markdown/);
  assert.match(rules, /\[\[happy\]\]/);
  assert.match(rules, /sleepy/);
  assert.ok(channelPlugin.agentPrompt.messageToolHints({}).join(' ').includes('kubik:'));
});

test('messaging target normalisation', () => {
  const { messaging } = channelPlugin;
  assert.equal(messaging.normalizeTarget('kubik:Kubik-B6C634'), DEVICE);
  assert.equal(messaging.normalizeTarget(DEVICE), DEVICE);
  assert.equal(messaging.normalizeTarget('not a device'), undefined);
  assert.deepEqual(messaging.parseExplicitTarget({ raw: `kubik:${DEVICE}` }), { to: DEVICE, chatType: 'direct' });
  assert.equal(messaging.inferTargetChatType({ to: DEVICE }), 'direct');
});

test('removed voice settings and device menu preferences are rejected', () => {
  for (const provider of ['openai-live', 'openai-realtime', 'openai-gpt-live']) {
    assert.throws(() => resolveAccount(config({ voice: { provider } }), 'default', { env: {} }), /voice\.provider.*not supported/);
  }
  assert.throws(() => resolveAccount(config({ voice: { model: 'gpt-realtime-2.1' } }), 'default', { env: {} }), /voice\.model is not supported/);
  assert.throws(() => resolveAccount(config({ devices: { [DEVICE]: { prefs: { engine: 'realtime' } } } }), 'default', { env: {} }), /devices\.kubik-b6c634.*unsupported setting/);
  assert.equal(channelSchema.properties.voice.properties.provider.enum.includes('openai-live'), false);
  assert.equal('prefs' in channelSchema.properties.devices.additionalProperties.properties, false);
  assert.equal('model' in channelSchema.properties.voice.properties, false);
});
