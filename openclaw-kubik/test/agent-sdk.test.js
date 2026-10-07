import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createServer } from 'node:net';
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { connectDevice, DEFAULT_DEVICE_KEY } from './helpers.js';
import { createActivityRelay, createCronRelay, defineAdapter, importAdapter, normalizeHostConfig, normalizeSetupValues, validateAdapter,
  writeHostConfig } from '../src/agent-sdk/index.js';
import { createAgentHost } from '../src/agent-sdk/host.js';
import { createFilePairingStore } from '../src/agent-sdk/pairing-store.js';
import { collectSetupValues } from '../src/agent-sdk/cli.js';

const adapterEnv = { API_SERVER_KEY: 'adapter-test-secret', OPENAI_API_KEY: 'voice-test-secret' };

function directory(t) {
  const path = mkdtempSync(join(tmpdir(), 'kubik-agent-sdk-'));
  t.after(() => rmSync(path, { recursive: true, force: true }));
  return path;
}

async function freePort() {
  const socket = createServer();
  socket.listen(0, '127.0.0.1');
  await once(socket, 'listening');
  const { port } = socket.address();
  await new Promise((resolve, reject) => socket.close((error) => error ? reject(error) : resolve()));
  return port;
}

test('custom native ESM adapter declares validated setup fields and only stores environment references for secrets', async (t) => {
  const modulePath = join(directory(t), 'custom-adapter.mjs');
  writeFileSync(modulePath, `export default {
    id: 'custom-test', label: 'Custom test',
    setup: [
      { key: 'endpoint', label: 'Endpoint', type: 'string', required: true, default: 'https://agent.example/v1' },
      { key: 'port', label: 'Port', type: 'integer', min: 1, max: 9000, default: 8000 },
      { key: 'tokenEnv', label: 'Token environment variable', type: 'env', default: 'CUSTOM_AGENT_TOKEN' }
    ],
    async connect({ config }) { this.config = config; },
    async start() {},
    async dispatch() {},
    async close() { this.config = null; }
  };\n`);
  const adapter = await importAdapter(modulePath);
  const answers = ['https://agent.example/v2', '9000', 'CUSTOM_AGENT_TOKEN'];
  const config = await collectSetupValues(adapter.setup, async () => answers.shift());
  assert.deepEqual(config, { endpoint: 'https://agent.example/v2', port: 9000, tokenEnv: 'CUSTOM_AGENT_TOKEN' });
  assert.throws(() => normalizeSetupValues(adapter.setup, { endpoint: 'https://agent.example/v1', port: 9001 }), /must be <= 9000/);
  assert.throws(() => normalizeSetupValues(adapter.setup, { endpoint: 'https://agent.example/v1', apiKey: 'do-not-save' }), /unknown adapter setup field/);
  assert.throws(() => validateAdapter({ ...adapter, setup: [{ key: 'apiKey', label: 'API key', type: 'string' }] }), /environment-variable name/);
  assert.equal(typeof adapter.connect, 'function');
});

test('setup field definitions reject misspelled or mistyped validation options', () => {
  const base = { id: 'setup-shape', label: 'Setup shape', setup: [], connect() {}, dispatch() {} };
  const field = { key: 'count', label: 'Count', type: 'integer', required: false, min: 1, max: 5, default: 3 };
  assert.doesNotThrow(() => validateAdapter({ ...base, setup: [field] }));
  assert.deepEqual(normalizeSetupValues([
    { key: 'name', label: 'Name', type: 'string', required: false, maxLength: 8, default: 'Kubik' },
    field,
  ]), { name: 'Kubik', count: 3 });
  assert.throws(() => validateAdapter({ ...base, setup: [{ ...field, required: 'false' }] }), /required must be true or false/);
  assert.throws(() => validateAdapter({ ...base, setup: [{ ...field, maxLenght: 4 }] }), /unknown property "maxLenght"/);
  assert.throws(() => validateAdapter({ ...base, setup: [{ ...field, maxLength: 4 }] }), /unknown property "maxLength"/);
  assert.throws(() => validateAdapter({ ...base, setup: [{ ...field, type: 'string', min: 1 }] }), /unknown property "min"/);
  assert.throws(() => validateAdapter({ ...base, setup: [{ key: 'name', label: 'Name', type: 'string', maxLength: null }] }), /invalid maxLength/);
  assert.throws(() => validateAdapter({ ...base, setup: [{ label: 'Name', type: 'string' }] }), /invalid key/);
  assert.throws(() => validateAdapter({ ...base, setup: [{ ...field, key: ['count'] }] }), /invalid key/);
  assert.throws(() => validateAdapter({ ...base, id: ['array-id'] }), /adapter id is invalid/);
});

test('activity and cron event relays validate snapshots, expire them, and keep bounded state', async () => {
  const changes = { activity: 0, cron: 0 };
  const activity = createActivityRelay({ onChange: () => changes.activity++ });
  activity.set('kubik-alpha', { own: 'web', other: '' }, 30_000);
  activity.set('kubik-beta', { own: 'coding', other: '' }, 30_000);
  assert.deepEqual(activity.summary(activity.sessionKeyFor('kubik-alpha')), { own: 'web', other: 'coding' });
  assert.throws(() => activity.set('kubik-alpha', { own: 'arbitrary', other: '' }), /Kubik activity category/);
  assert.throws(() => activity.set('kubik-alpha', { own: 'web', other: '' }, 60_001), /event TTL/);
  for (let index = 0; index < 130; index++) activity.set(`device-${index}`, { own: 'thinking', other: '' }, 30_000);
  assert.equal(activity.size, 128);
  assert.equal(activity.summary(activity.sessionKeyFor('kubik-alpha')).own, '');

  const cron = createCronRelay({ onChange: () => changes.cron++ });
  assert.deepEqual(cron.set({ running: 2, next: 120 }, 1000), { running: 2, next: 120 });
  assert.deepEqual(cron.summary(), { running: 2, next: 120 });
  cron.set({ running: 2, next: 120 }, 1000);
  assert.ok(changes.cron >= 2, 'repeated snapshots refresh devices and extend the cron TTL');
  assert.throws(() => cron.set({ running: -1, next: 10 }), /cron snapshot/);
  await new Promise((resolve) => setTimeout(resolve, 1050));
  assert.deepEqual(cron.summary(), { running: 0, next: -1 });
  assert.ok(changes.activity > 0);
  assert.ok(changes.cron >= 2);
  activity.close();
  cron.close();
});

test('pairing requests persist across stores and require explicit approval by their displayed code', async (t) => {
  const stateDir = directory(t);
  const first = await createFilePairingStore(stateDir, { randomCode: () => 'ABCDEFG2' }).ready();
  const second = await createFilePairingStore(stateDir).ready();
  const { entry } = DEFAULT_DEVICE_KEY;
  const request = await first.upsert(entry, { name: 'Кубик', fw: '0.6.1' });
  assert.deepEqual(request, { code: 'ABCDEFG2', created: true });
  assert.deepEqual(await first.allowed(), [], 'an incoming request must not approve itself');
  assert.deepEqual((await second.list()).pending.map(({ entry: item, code }) => ({ entry: item, code })), [{ entry, code: 'ABCDEFG2' }]);
  const approved = await second.approve('abcdefg2');
  assert.equal(approved.entry, entry);
  assert.deepEqual(await first.allowed(), [entry], 'the active host sees approval made by a separate CLI process');
  const restarted = await createFilePairingStore(stateDir).ready();
  assert.deepEqual((await restarted.list()).approved, [entry]);
  await assert.rejects(restarted.approve('ABCDEFG2'), /no unexpired pairing request/);
  if (process.platform !== 'win32') {
    assert.equal(statSync(join(stateDir, 'pairings.json')).mode & 0o777, 0o600);
    assert.equal(statSync(stateDir).mode & 0o777, 0o700);
  }
});

test('standalone host authenticates and pairs over the existing TLS WebSocket protocol', { timeout: 10_000 }, async (t) => {
  const stateDir = directory(t);
  const port = await freePort();
  let receivedEvents;
  const selected = new Map();
  const choices = Array.from({length: 6}, (_, i) => ({id: `model-${i}`, label: `Model ${i}`}));
  const adapter = validateAdapter({
    id: 'test-host', label: 'Test host', setup: [],
    async modelOptions(device) { return {model: selected.get(device.id) || 'model-0', models: choices}; },
    async selectModel(device, id) { selected.set(device.id, id); },
    async connect() {},
    async start({ events }) { receivedEvents = events; },
    async dispatch({ speak, transcript }) { await speak(`Ответ на ${transcript}`); },
    async close() {},
  });
  const config = normalizeHostConfig({ version: 2, listener: { host: '127.0.0.1', port }, voice: { baseUrl: 'https://speech.example/v1', transcribeModel: 'custom-stt', ttsModel: 'custom-tts', voice: 'custom-voice', language: 'en' },
    adapter: { id: adapter.id, setup: {} } });
  const host = await createAgentHost({ stateDir, config, adapter, env: adapterEnv,
    lanOptions: { host: '127.0.0.1', discoveryHost: '127.0.0.1', discoveryPort: 0 },
    serverOptions: { pairingPollMs: 20, pairingTimeoutMs: 3000 } });
  let device;
  t.after(async () => { device?.close(); await host.close(); });

  assert.equal(host.account.voice.provider, 'openai-http');
  for (const [key, value] of Object.entries(config.voice)) assert.equal(host.account.voice[key], value);
  assert.deepEqual(host.listener.status.addresses, [], 'loopback listener never advertises unrelated NICs');
  assert.equal(typeof receivedEvents.activity, 'function');
  assert.equal(typeof receivedEvents.cron, 'function');
  const activitySnapshot = receivedEvents.activity(DEFAULT_DEVICE_KEY.device, { own: 'coding', other: '' }, 60_000);
  assert.deepEqual({ deviceId: activitySnapshot.deviceId, own: activitySnapshot.own, other: activitySnapshot.other }, {
    deviceId: DEFAULT_DEVICE_KEY.device, own: 'coding', other: '',
  });
  assert.deepEqual(host.server.activity.summary(host.server.activity.sessionKeyFor(DEFAULT_DEVICE_KEY.device)), { own: 'coding', other: '' });
  assert.equal(host.server.cron.summary().running, 0);
  assert.deepEqual(receivedEvents.cron({ running: 2, next: 60 }, 60_000), { running: 2, next: 60 });
  assert.deepEqual(host.server.cron.summary(), { running: 2, next: 60 });
  assert.equal(typeof receivedEvents.notify, 'function');
  assert.equal(host.listener.port, port);
  device = await connectDevice(`wss://127.0.0.1:${port}/kubik/v1`, { bind: 'seen' });
  const pair = device.events.find((event) => event.t === 'pair');
  assert.ok(pair?.code, 'unapproved device receives a pairing code and no session');
  assert.deepEqual(host.server.onlineDevices, []);
  const cliPairing = await createFilePairingStore(stateDir).ready();
  await cliPairing.approve(pair.code);
  const welcome = await device.waitFor((event) => event.t === 'welcome');
  assert.equal(welcome.t, 'welcome');
  assert.deepEqual(host.server.onlineDevices, [DEFAULT_DEVICE_KEY.device]);
  assert.equal(host.server.getSession(DEFAULT_DEVICE_KEY.device).device.fingerprint, DEFAULT_DEVICE_KEY.fingerprint);
  device.send({t: 'agent_options', target: 'agent', rid: 60, cursor: 1});
  const options = await device.waitFor(e => e.t === 'agent_options' && e.rid === 60);
  assert.deepEqual(options.models, choices.slice(4));
  assert.equal(options.stt.available, true); assert.equal(options.tts.available, true);
  device.send({t: 'agent_model', target: 'agent', rid: 61, cursor: 1, id: 'model-5'});
  assert.equal((await device.waitFor(e => e.t === 'agent_options' && e.rid === 61)).model, 'model-5');
  assert.equal(selected.size, 1, 'only this device has an override');
  device.send({t: 'agent_model', target: 'agent', rid: 62, cursor: 0, id: 'unknown'});
  assert.equal((await device.waitFor(e => e.t === 'agent_options' && e.rid === 62)).error, 'invalid_model');

  assert.deepEqual(await device.waitFor((event) => event.t === 'activity'), { t: 'activity', own: 'coding', other: '' });
  assert.deepEqual(await device.waitFor((event) => event.t === 'cron'), { t: 'cron', running: 2, next: 60 });
  assert.ok(readFileSync(join(stateDir, 'lan-tls.json'), 'utf8').includes('BEGIN PRIVATE KEY'));

  device.close();
  await device.closed;
  device = await connectDevice(`wss://127.0.0.1:${port}/kubik/v1`, { bind: 'seen' });
  assert.ok(device.events.some((event) => event.t === 'welcome'), 'approved device reconnects without a new pairing');
  device.send({t: 'agent_options', target: 'agent', rid: 63, cursor: 0});
  assert.equal((await device.waitFor(e => e.t === 'agent_options' && e.rid === 63)).model, 'model-5');
  assert.deepEqual(await device.waitFor((event) => event.t === 'activity'), { t: 'activity', own: 'coding', other: '' });
  assert.deepEqual(await device.waitFor((event) => event.t === 'cron'), { t: 'cron', running: 2, next: 60 });

  device.close();
  await device.closed;
  const notification = await receivedEvents.notify(DEFAULT_DEVICE_KEY.device, 'Проверочное уведомление');
  assert.equal(notification.status, 'queued');
  assert.equal(notification.durable, true);
  assert.ok(readFileSync(join(stateDir, 'notifications.json'), 'utf8').includes('Проверочное уведомление'));
  await host.close();
  const queueBefore = readFileSync(join(stateDir, 'notifications.json'), 'utf8');
  assert.throws(() => receivedEvents.activity(DEFAULT_DEVICE_KEY.device, { own: 'coding' }), /host is closed/);
  assert.throws(() => receivedEvents.cron({ running: 1, next: 5 }), /host is closed/);
  assert.throws(() => receivedEvents.notify(DEFAULT_DEVICE_KEY.device, 'После закрытия'), /host is closed/);
  assert.equal(readFileSync(join(stateDir, 'notifications.json'), 'utf8'), queueBefore);

});

test('host configuration remains private and does not accept plaintext credentials as setup values', async (t) => {
  const stateDir = directory(t);
  const config = normalizeHostConfig({ version: 2, listener: { port: 18790 }, voice: { baseUrl: 'https://api.openai.com/v1' },
    adapter: { id: 'custom-fixture', setup: { apiKeyEnv: 'API_SERVER_KEY' } } });
  await writeHostConfig(stateDir, config);
  assert.equal(statSync(join(stateDir, 'agent-host.json')).mode & 0o777, 0o600);
  assert.doesNotMatch(readFileSync(join(stateDir, 'agent-host.json'), 'utf8'), /adapter-test-secret|voice-test-secret/);
  assert.throws(() => normalizeSetupValues([{ key: 'token', label: 'Access token', type: 'string' }], { token: 'plain-secret' }), /environment-variable name/);
});


test('failed adapter startup closes its retained event surface', async (t) => {
  const stateDir = directory(t);
  const port = await freePort();
  let retained;
  const adapter = validateAdapter({
    id: 'failed-host', label: 'Failed host', setup: [],
    async connect() {}, async dispatch() {}, async close() {},
    async start({ events }) { retained = events; throw new Error('startup failed'); },
  });
  const config = normalizeHostConfig({ version: 2, listener: { host: '127.0.0.1', port }, voice: { baseUrl: 'https://api.openai.com/v1' },
    adapter: { id: adapter.id, setup: {} } });
  await assert.rejects(createAgentHost({ stateDir, config, adapter, env: adapterEnv,
    lanOptions: { host: '127.0.0.1', discoveryHost: '127.0.0.1', discoveryPort: 0 } }), /startup failed/);
  assert.throws(() => retained.activity(DEFAULT_DEVICE_KEY.device, { own: 'coding' }), /host is closed/);
  assert.throws(() => retained.cron({ running: 1, next: 5 }), /host is closed/);
  assert.throws(() => retained.notify(DEFAULT_DEVICE_KEY.device, 'После ошибки'), /host is closed/);
});

test('defineAdapter speaks a reply or ordered fragments and reports a bad reply', async () => {
  const spoken = [];
  const errors = [];
  const adapter = defineAdapter({
    id: 'echo', label: 'Echo', setup: [],
    async connect() {},
    async reply({ transcript }) {
      if (transcript === 'parts') return ['One.', 'Two.'];
      if (transcript === 'stream') return (async function* () { yield 'A.'; yield 'B.'; })();
      if (transcript === 'bad') return 1;
      return `Echo ${transcript}`;
    },
  });
  const turn = (transcript) => adapter.dispatch({
    transcript, isCurrent: () => true, speak: async (text) => spoken.push(text),
    onAgentError: (error) => errors.push(error.message),
  });
  await adapter.connect({});
  await turn('hi');
  await turn('parts');
  await turn('stream');
  await turn('bad');
  assert.deepEqual(spoken, ['Echo hi', 'One.', 'Two.', 'A.', 'B.']);
  assert.deepEqual(errors, ['adapter reply must return text or text fragments']);
  await adapter.close();
});

test('interrupting speech does not abort the agent, and close does', async () => {
  let release;
  let signal;
  let current = true;
  const spoken = [];
  const errors = [];
  const adapter = defineAdapter({
    id: 'slow', label: 'Slow', setup: [],
    async connect() {},
    async reply({ signal: lifetime }) {
      signal = lifetime;
      await new Promise((resolve) => { release = resolve; });
      if (lifetime.aborted) return;
      return 'late';
    },
  });
  await adapter.connect({});
  const job = adapter.dispatch({
    transcript: 'wait', isCurrent: () => current, speak: async (text) => spoken.push(text),
    onAgentError: (error) => errors.push(error.message),
  });
  assert.equal(typeof release, 'function');
  current = false;
  assert.equal(signal.aborted, false);
  release();
  await job;
  assert.deepEqual(spoken, []);
  await adapter.close();
  assert.equal(signal.aborted, true);
  assert.deepEqual(errors, []);
});

test('defineAdapter accepts connect and reply, not a hand-written dispatch', () => {
  const spec = { id: 'echo', label: 'Echo', setup: [], connect() {}, reply() { return 'ok'; } };
  assert.equal(typeof defineAdapter(spec).dispatch, 'function');
  assert.throws(() => defineAdapter({ ...spec, dispatch() {} }), /reply\(\), not dispatch/);
  assert.throws(() => defineAdapter({ ...spec, reply: undefined }), /connect\(\) and reply/);
});

test('a streamed reply finishes after speech is interrupted', async () => {
  let continued = false;
  let current = true;
  const spoken = [];
  const adapter = defineAdapter({
    id: 'stream', label: 'Stream', setup: [],
    async connect() {},
    async *reply() {
      yield 'One.';
      await new Promise((resolve) => setTimeout(resolve, 10));
      continued = true;
      yield 'Two.';
    },
  });
  await adapter.connect({});
  await adapter.dispatch({
    transcript: 'x', isCurrent: () => current,
    speak: async (text) => { spoken.push(text); current = false; },
  });
  assert.equal(continued, true);
  assert.deepEqual(spoken, ['One.']);
  await adapter.close();
});

test('an older connect failure does not abort or outlive the connection that replaced it', async () => {
  let release;
  let generation = 0;
  const signals = [];
  const adapter = defineAdapter({
    id: 'race', label: 'Race', setup: [],
    connect({ signal }) {
      signals.push(signal);
      generation += 1;
      if (generation === 1) return new Promise((_resolve, reject) => { release = () => reject(new Error('old')); });
    },
    reply() { return 'ok'; },
  });
  const first = adapter.connect({});
  const second = adapter.connect({});
  await second;
  release();
  await assert.rejects(first, /old/);
  assert.equal(signals[0].aborted, true);
  assert.equal(signals[1].aborted, false);
  await adapter.close();
  assert.equal(signals[1].aborted, true);
  const spoken = [];
  await adapter.dispatch({ transcript: 'x', isCurrent: () => true, speak: async (text) => spoken.push(text) });
  assert.deepEqual(spoken, []);
});

test('close during connect stays closed', async () => {
  let release;
  let signal;
  const adapter = defineAdapter({
    id: 'race', label: 'Race', setup: [],
    connect({ signal: current }) {
      signal = current;
      return new Promise((_resolve, reject) => { release = () => reject(new Error('closed')); });
    },
    reply() { return 'ok'; },
  });
  const pending = adapter.connect({});
  await adapter.close();
  release();
  await assert.rejects(pending, /closed/);
  assert.equal(signal.aborted, true);
  const spoken = [];
  await adapter.dispatch({ transcript: 'x', isCurrent: () => true, speak: async (text) => spoken.push(text) });
  assert.deepEqual(spoken, []);
});

test('defineAdapter bounds single replies, aggregate text and empty producers before enqueueing', async () => {
  let reads = 0, closed = false;
  const adapter = defineAdapter({ id: 'bounded', label: 'Bounded', connect() {}, reply({ transcript }) {
    if (transcript === 'limit') return 'x'.repeat(8192);
    if (transcript === 'large') return 'x'.repeat(8193);
    if (transcript === 'parts') return ['x'.repeat(4096), 'x'.repeat(4096), 'extra'];
    return (function* () { try { while (true) { reads++; yield ''; } } finally { closed = true; } })();
  } });
  await adapter.connect();
  for (const transcript of ['limit', 'large', 'parts', 'empty']) {
    const spoken = [], errors = [];
    await adapter.dispatch({ transcript, isCurrent: () => true, speak: async (text) => spoken.push(text),
      onAgentError: (error) => errors.push(error.message) });
    assert.equal(spoken.join('').length, ['limit', 'parts'].includes(transcript) ? 8192 : 0);
    assert.equal(errors.length, transcript === 'limit' ? 0 : 1);
    if (errors.length) assert.match(errors[0], /8192 characters or 512 fragments/);
  }
  assert.equal(reads, 513); assert.equal(closed, true);
  await adapter.close();
});
