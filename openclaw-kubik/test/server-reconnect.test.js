import assert from 'node:assert/strict';
import { once } from 'node:events';
import test from 'node:test';
import { OpenClawEngine } from '../src/engines/openclaw.js';
import { connectDevice, DEFAULT_DEVICE_KEY, deviceKey, FAKE_GATEWAY_BIND, fakeEngine, fakePairing } from './helpers.js';
import { start } from './server-fixture.js';

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

async function concurrentRefreshOrder(t, order) {
  const engine = fakeEngine();
  const { server, url } = await start(t, { engine });
  const previous = await connectDevice(url);
  const refreshes = [], ready = deferred();
  engine.prepareCapabilities = () => {
    const refresh = deferred();
    refreshes.push(refresh);
    if (refreshes.length === 2) ready.resolve();
    return refresh.promise;
  };
  engine.applyCapabilities = snapshot => { engine.appliedCapabilities = snapshot; };
  t.after(() => { for (const refresh of refreshes) refresh.resolve(); });
  const first = await connectDevice(url, { awaitAuth: false });
  const second = await connectDevice(url, { awaitAuth: false });
  t.after(() => { previous.close(); first.close(); second.close(); });
  await ready.promise;
  const candidates = [first, second];
  let committed = -1;
  for (const index of order) {
    refreshes[index].resolve();
    if (index < committed) {
      assert.equal(await candidates[index].closed, 4003, 'a stale successful candidate is released as replaced');
      assert.equal(candidates[index].events.some(event => event.t === 'welcome'), false);
      continue;
    }
    await candidates[index].waitFor(event => event.t === 'welcome');
    committed = index;
  }
  assert.equal(await previous.closed, 4003);
  assert.equal(await first.closed, 4003, 'the older candidate is released after the newer one commits');
  const newer = second.events.find(event => event.t === 'welcome');
  assert.ok(newer);
  assert.equal(first.events.some(event => event.t === 'welcome'), order[0] === 0,
    'the older candidate only gets a welcome if its refresh commits before the newer one');
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).sessionId, newer.session);
  second.send({ t: 'ping', ts: 99 });
  await second.waitFor(event => event.t === 'pong' && event.ts === 99);
  assert.deepEqual(server.onlineDevices, [DEFAULT_DEVICE_KEY.device]);
}

test('concurrent capability refreshes preserve the newest admitted connection in either completion order', async t => {
  await t.test('refreshes finish in admission order', { timeout: 10_000 }, async t => concurrentRefreshOrder(t, [0, 1]));
  await t.test('older refresh finishes last', { timeout: 10_000 }, async t => concurrentRefreshOrder(t, [1, 0]));
});

test('a failed newer refresh preserves the live session and does not block an older successful candidate', { timeout: 10_000 }, async t => {
  const engine = fakeEngine();
  const { server, url } = await start(t, { engine });
  const previous = await connectDevice(url);
  const oldSession = server.getSession(DEFAULT_DEVICE_KEY.device);
  const refreshes = [], ready = deferred();
  engine.prepareCapabilities = () => {
    const refresh = deferred();
    refreshes.push(refresh);
    if (refreshes.length === 2) ready.resolve();
    return refresh.promise;
  };
  engine.applyCapabilities = snapshot => { engine.appliedCapabilities = snapshot; };
  const first = await connectDevice(url, { awaitAuth: false });
  const second = await connectDevice(url, { awaitAuth: false });
  t.after(() => { previous.close(); first.close(); second.close(); });
  t.after(() => { for (const refresh of refreshes) refresh.resolve(); });
  await ready.promise;
  refreshes[1].reject(Error('capability refresh failed'));
  assert.equal(await second.closed, 1011);
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device), oldSession);
  previous.send({ t: 'ping', ts: 98 });
  await previous.waitFor(event => event.t === 'pong' && event.ts === 98);

  refreshes[0].resolve();
  const welcome = await first.waitFor(event => event.t === 'welcome');
  assert.equal(await previous.closed, 4003);
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).sessionId, welcome.session);
});

async function failedReconnectRecovery(t, reconnectKey) {
  const pairing = fakePairing({ allowed: [...new Set([DEFAULT_DEVICE_KEY.entry, reconnectKey.entry])] });
  const engine = fakeEngine();
  let preparations = 0;
  engine.prepareCapabilities = async ({ agentId, getVoice }) => {
    const voice = await getVoice();
    if (++preparations === 2) throw new Error('candidate capability lookup failed');
    return { agentId, voice, getVoice, capabilities: { canListen: false, canSpeak: false } };
  };
  engine.applyCapabilities = snapshot => Object.assign(engine, snapshot);
  const agentControl = {
    agentId: device => `agent-${device.fingerprint}`,
    voiceSettings: async device => ({ fingerprint: device.fingerprint }),
  };
  const { server, url } = await start(t, { pairing, engine, serverOptions: { agentControl } });
  const first = await connectDevice(url);
  t.after(() => first.close());
  await first.waitFor(event => event.t === 'welcome');
  const oldResolver = engine.getVoice;
  assert.equal((await oldResolver()).fingerprint, DEFAULT_DEVICE_KEY.fingerprint);
  // The client can close before the server has removed its session.
  const serverClosed = once(server.getSession(DEFAULT_DEVICE_KEY.device).ws, 'close');
  first.close();
  await Promise.all([first.closed, serverClosed]);
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device), undefined);

  const reconnectOptions = { hello: reconnectKey.hello(),
    auth: nonce => reconnectKey.sign(nonce, { bind: FAKE_GATEWAY_BIND }), awaitAuth: false };
  const reconnect = await connectDevice(url, reconnectOptions);
  t.after(() => reconnect.close());
  const result = await Promise.race([
    reconnect.waitFor(event => event.t === 'welcome').then(() => 'welcome'),
    reconnect.closed.then(code => `closed:${code}`),
  ]);
  assert.equal(result, 'closed:1011', 'a reconnect must not inherit a stale cached voice snapshot');
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device), undefined);
  assert.equal(engine.getVoice, oldResolver);

  const recovered = await connectDevice(url, reconnectOptions);
  t.after(() => recovered.close());
  await recovered.waitFor(event => event.t === 'welcome');
  assert.notEqual(engine.getVoice, oldResolver, 'a later successful refresh binds the recovered session');
  assert.equal((await engine.getVoice()).fingerprint, reconnectKey.fingerprint);
}

test('a failed reconnect cannot reuse a cached voice resolver, then a successful retry recovers', { timeout: 20_000 }, async t => {
  await t.test('same fingerprint', { timeout: 10_000 }, async t => failedReconnectRecovery(t, DEFAULT_DEVICE_KEY));
  await t.test('rotated fingerprint', { timeout: 10_000 }, async t => failedReconnectRecovery(t, deviceKey(DEFAULT_DEVICE_KEY.device)));
});

test('an accepted initial session keeps its admission priority when capability preparation fails', { timeout: 10_000 }, async t => {
  const engine = fakeEngine();
  const { server, url } = await start(t, { engine });
  const refreshes = [], ready = deferred();
  engine.prepareCapabilities = () => {
    const refresh = deferred();
    refreshes.push(refresh);
    if (refreshes.length === 2) ready.resolve();
    return refresh.promise;
  };
  engine.applyCapabilities = snapshot => { engine.appliedCapabilities = snapshot; };
  t.after(() => {
    for (const refresh of refreshes) refresh.resolve();
  });
  const older = await connectDevice(url, { awaitAuth: false });
  const newer = await connectDevice(url, { awaitAuth: false });
  t.after(() => { older.close(); newer.close(); });
  await ready.promise;

  refreshes[1].reject(Error('capability preparation failed'));
  const welcome = await newer.waitFor(event => event.t === 'welcome');
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).sessionId, welcome.session);

  refreshes[0].resolve();
  assert.equal(await older.closed, 4003, 'an older candidate cannot replace the committed degraded session');
  assert.equal(older.events.some(event => event.t === 'welcome'), false);
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).sessionId, welcome.session);
  newer.send({ t: 'ping', ts: 97 });
  await newer.waitFor(event => event.t === 'pong' && event.ts === 97);
});

test('a closed reconnect cannot commit voice capabilities for a different device fingerprint', { timeout: 10_000 }, async t => {
  const configured = deferred(), entered = deferred();
  const sdk = { getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1'], resolveConfig: ({ rawConfig }) => rawConfig,
    isConfigured: () => { entered.resolve(); return configured.promise; } }),
  resolveRealtimeVoiceProviderCapabilities: () => ({}) };
  const otherKey = deviceKey(DEFAULT_DEVICE_KEY.device);
  const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry, otherKey.entry] });
  const preparations = [], refreshes = [];
  let engine;
  const oldVoice = { provider: 'openclaw', mode: 'classic', voice: 'old-device' };
  const candidateVoice = { provider: 'openclaw', mode: 'realtime', voice: 'uncommitted-device' };
  const agentControl = {
    agentId: device => `agent-${device.fingerprint}`,
    voiceSettings: async device => device.fingerprint === DEFAULT_DEVICE_KEY.fingerprint ? oldVoice : candidateVoice,
  };
  const { server, url } = await start(t, { pairing, settings: { voice: { provider: 'openclaw', mode: 'classic' } },
    engineFactory: (voice, options) => {
      engine = new OpenClawEngine(voice, { ...options, nativeOptions: { sdk } });
      const prepare = engine.prepareCapabilities.bind(engine);
      engine.prepareCapabilities = settings => {
        const pending = prepare(settings);
        preparations.push({ settings, pending });
        return pending;
      };
      const refresh = engine.refreshCapabilities.bind(engine);
      engine.refreshCapabilities = settings => {
        const pending = refresh(settings);
        refreshes.push({ settings, pending });
        return pending;
      };
      return engine;
    }, serverOptions: { agentControl } });
  const previous = await connectDevice(url);
  t.after(() => previous.close());
  await previous.waitFor(event => event.t === 'welcome');
  assert.equal(engine.voice.voice, 'old-device');
  const currentResolver = preparations[0].settings.getVoice;
  assert.equal(engine.getVoice, currentResolver, 'successful admission owns the engine resolver');
  const oldSession = server.getSession(DEFAULT_DEVICE_KEY.device);

  const candidate = await connectDevice(url, { hello: otherKey.hello(),
    auth: nonce => otherKey.sign(nonce, { bind: FAKE_GATEWAY_BIND }), awaitAuth: false });
  t.after(() => candidate.close());
  await entered.promise;
  const candidateRefresh = preparations[1];
  assert.equal(candidateRefresh.settings.getVoice instanceof Function, true);
  assert.notEqual(candidateRefresh.settings.getVoice, currentResolver);
  candidate.close();
  await candidate.closed;
  configured.resolve(true);
  await candidateRefresh.pending;

  assert.equal(engine.voice.voice, 'old-device');
  assert.equal(engine.voiceMode, 'classic');
  assert.equal(engine.agentId, `agent-${DEFAULT_DEVICE_KEY.fingerprint}`);
  assert.equal(engine.canListen, false);
  assert.equal(engine.getVoice, currentResolver, 'the shared engine remains bound to the live session');
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device), oldSession);
  previous.send({ t: 'ping', ts: 101 });
  await previous.waitFor(event => event.t === 'pong' && event.ts === 101);
});

test('a voice write finishing after reconnect refreshes the current session voice profile', { timeout: 10_000 }, async t => {
  const currentKey = deviceKey(DEFAULT_DEVICE_KEY.device);
  const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry, currentKey.entry] });
  const sdk = { getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1'], resolveConfig: ({ rawConfig }) => rawConfig,
    isConfigured: () => true }), resolveRealtimeVoiceProviderCapabilities: () => ({}) };
  const started = deferred(), write = deferred();
  t.after(() => write.resolve());
  const selected = new Map();
  const agentControl = {
    agentId: device => `agent-${device.fingerprint}`,
    voiceSettings: async device => ({ provider: 'openclaw', mode: selected.get(device.id) ?? 'classic' }),
    async selectModel(device, id, target) {
      if (target === 'voice') { started.resolve(); await write.promise; selected.set(device.id, id); }
    },
    async options() { return { model: '', models: [] }; },
  };
  const preparations = [], refreshes = [];
  let engine;
  const { server, url } = await start(t, { pairing, settings: { voice: { provider: 'openclaw', mode: 'classic' } },
    engineFactory: (voice, options) => {
      engine = new OpenClawEngine(voice, { ...options, nativeOptions: { sdk } });
      const prepare = engine.prepareCapabilities.bind(engine);
      engine.prepareCapabilities = settings => {
        const pending = prepare(settings);
        preparations.push({ settings, pending });
        return pending;
      };
      const refresh = engine.refreshCapabilities.bind(engine);
      engine.refreshCapabilities = settings => {
        const pending = refresh(settings);
        refreshes.push({ settings, pending });
        return pending;
      };
      return engine;
    },
    serverOptions: { agentControl } });
  const previous = await connectDevice(url);
  t.after(() => previous.close());
  await previous.waitFor(event => event.t === 'welcome');
  previous.send({ t: 'agent_model', target: 'voice', rid: 111, cursor: 0, id: 'realtime' });
  await started.promise;

  const current = await connectDevice(url, { hello: currentKey.hello(),
    auth: nonce => currentKey.sign(nonce, { bind: FAKE_GATEWAY_BIND }) });
  t.after(() => current.close());
  await current.waitFor(event => event.t === 'welcome');
  assert.equal(await previous.closed, 4003);
  assert.equal(engine.voiceMode, 'classic', 'the new connection initially reads the not-yet-saved profile');
  const currentResolver = engine.getVoice;
  assert.equal(typeof currentResolver, 'function');

  write.resolve();
  for (let attempt = 0; refreshes.length < 1 && attempt < 100; attempt++) await new Promise(resolve => setImmediate(resolve));
  assert.equal(refreshes.length, 1, 'the completed selection starts a fresh current-session capability read');
  await refreshes[0].pending;
  assert.equal(engine.voiceMode, 'realtime', 'the old request re-reads through the resolver committed by the current session');
  assert.equal(engine.canListen, true);
  assert.equal(engine.getVoice, currentResolver);
  assert.equal(engine.agentId, `agent-${currentKey.fingerprint}`, 'the old control keeps the current session agent identity');
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).engine, engine);
});

test('a newer admitted candidate commits its voice snapshot after an older control refresh', { timeout: 10_000 }, async t => {
  const otherKey = deviceKey(DEFAULT_DEVICE_KEY.device);
  const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry, otherKey.entry] });
  const candidateVoice = deferred(), candidateReading = deferred();
  const sdk = { getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1'], resolveConfig: ({ rawConfig }) => rawConfig,
    isConfigured: () => true }), resolveRealtimeVoiceProviderCapabilities: () => ({}) };
  const started = deferred(), write = deferred();
  t.after(() => { write.resolve(); candidateVoice.resolve({ provider: 'openclaw', mode: 'classic', voice: 'candidate-device' }); });
  const selected = new Map();
  const oldProfile = { mode: 'classic', voice: 'old-device' };
  const agentControl = {
    agentId: device => `agent-${device.fingerprint}`,
    async voiceSettings(device) {
      if (device.fingerprint === otherKey.fingerprint) {
        candidateReading.resolve();
        return candidateVoice.promise;
      }
      return { provider: 'openclaw', ...oldProfile, ...(selected.get(device.fingerprint) ? { mode: selected.get(device.fingerprint) } : {}) };
    },
    async selectModel(device, id, target) {
      if (target === 'voice') { started.resolve(); await write.promise; selected.set(device.fingerprint, id); }
    },
    async options() { return { model: '', models: [] }; },
  };
  const preparations = [], refreshes = [];
  let engine;
  const { server, url } = await start(t, { pairing, settings: { voice: { provider: 'openclaw', mode: 'classic' } },
    engineFactory: (voice, options) => {
      engine = new OpenClawEngine(voice, { ...options, nativeOptions: { sdk } });
      const prepare = engine.prepareCapabilities.bind(engine);
      engine.prepareCapabilities = settings => {
        const pending = prepare(settings);
        preparations.push({ settings, pending });
        return pending;
      };
      const refresh = engine.refreshCapabilities.bind(engine);
      engine.refreshCapabilities = settings => {
        const pending = refresh(settings);
        refreshes.push({ settings, pending });
        return pending;
      };
      return engine;
    }, serverOptions: { agentControl } });
  const previous = await connectDevice(url);
  t.after(() => previous.close());
  await previous.waitFor(event => event.t === 'welcome');
  previous.send({ t: 'agent_model', target: 'voice', rid: 112, cursor: 0, id: 'realtime' });
  await started.promise;

  const candidate = await connectDevice(url, { hello: otherKey.hello(),
    auth: nonce => otherKey.sign(nonce, { bind: FAKE_GATEWAY_BIND }), awaitAuth: false });
  t.after(() => candidate.close());
  await candidateReading.promise;
  assert.equal(preparations.length, 2, 'initial session and candidate refreshes are staged');

  write.resolve();
  for (let attempt = 0; refreshes.length < 1 && attempt < 100; attempt++) await new Promise(resolve => setImmediate(resolve));
  assert.equal(refreshes.length, 1, 'the old session refresh starts after its voice write');
  await refreshes[0].pending;
  assert.equal(engine.voice.voice, 'old-device');
  assert.equal(engine.voiceMode, 'realtime');

  candidateVoice.resolve({ provider: 'openclaw', mode: 'classic', voice: 'candidate-device' });
  await preparations[1].pending;
  const welcome = await candidate.waitFor(event => event.t === 'welcome');
  assert.equal(engine.voice.voice, 'candidate-device', 'the admitted connection uses its own committed snapshot');
  assert.equal(engine.voiceMode, 'classic');
  assert.equal(engine.agentId, `agent-${otherKey.fingerprint}`);
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).sessionId, welcome.session);
  assert.equal(await previous.closed, 4003);
});

async function simultaneousCapabilityRefreshes(t, releaseOrder) {
  const otherKey = deviceKey(DEFAULT_DEVICE_KEY.device);
  const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry, otherKey.entry] });
  const candidateVoice = deferred(), candidateReading = deferred();
  const controlVoice = deferred(), controlReading = deferred();
  const sdk = { getRealtimeVoiceProvider: () => ({ models: ['gpt-realtime-2.1'], resolveConfig: ({ rawConfig }) => rawConfig,
    isConfigured: () => true }), resolveRealtimeVoiceProviderCapabilities: () => ({}) };
  const started = deferred(), write = deferred();
  t.after(() => {
    write.resolve();
    candidateVoice.resolve({ provider: 'openclaw', mode: 'classic', voice: 'candidate-device' });
    controlVoice.resolve({ provider: 'openclaw', mode: 'realtime', voice: 'old-device' });
  });
  const selected = new Map();
  const agentControl = {
    agentId: device => `agent-${device.fingerprint}`,
    async voiceSettings(device) {
      if (device.fingerprint === otherKey.fingerprint) {
        candidateReading.resolve();
        return candidateVoice.promise;
      }
      if (selected.has(device.fingerprint)) {
        controlReading.resolve();
        return controlVoice.promise;
      }
      return { provider: 'openclaw', mode: 'classic', voice: 'old-device' };
    },
    async selectModel(device, id, target) {
      if (target === 'voice') { started.resolve(); await write.promise; selected.set(device.fingerprint, id); }
    },
    async options() { return { model: '', models: [] }; },
  };
  const preparations = [], refreshes = [];
  let engine;
  const { server, url } = await start(t, { pairing, settings: { voice: { provider: 'openclaw', mode: 'classic' } },
    engineFactory: (voice, options) => {
      engine = new OpenClawEngine(voice, { ...options, nativeOptions: { sdk } });
      const prepare = engine.prepareCapabilities.bind(engine);
      engine.prepareCapabilities = settings => {
        const pending = prepare(settings);
        preparations.push({ settings, pending });
        return pending;
      };
      const refresh = engine.refreshCapabilities.bind(engine);
      engine.refreshCapabilities = settings => {
        const pending = refresh(settings);
        refreshes.push({ settings, pending });
        return pending;
      };
      return engine;
    }, serverOptions: { agentControl } });
  const previous = await connectDevice(url);
  t.after(() => previous.close());
  await previous.waitFor(event => event.t === 'welcome');
  previous.send({ t: 'agent_model', target: 'voice', rid: 113, cursor: 0, id: 'realtime' });
  await started.promise;

  const candidate = await connectDevice(url, { hello: otherKey.hello(),
    auth: nonce => otherKey.sign(nonce, { bind: FAKE_GATEWAY_BIND }), awaitAuth: false });
  t.after(() => candidate.close());
  await candidateReading.promise;
  write.resolve();
  await controlReading.promise;
  assert.equal(preparations.length, 3, 'initial, candidate, and control snapshots are prepared');
  assert.equal(refreshes.length, 1);

  for (const index of releaseOrder) {
    if (index === 0) candidateVoice.resolve({ provider: 'openclaw', mode: 'classic', voice: 'candidate-device' });
    else controlVoice.resolve({ provider: 'openclaw', mode: 'realtime', voice: 'old-device' });
  }
  const welcome = await candidate.waitFor(event => event.t === 'welcome');
  await refreshes[0].pending;
  assert.equal(engine.voice.voice, 'candidate-device', 'the accepted candidate snapshot wins both completion orders');
  assert.equal(engine.voiceMode, 'classic');
  assert.equal(engine.agentId, `agent-${otherKey.fingerprint}`);
  assert.equal(engine.getVoice, preparations[1].settings.getVoice);
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).sessionId, welcome.session);
  assert.equal(await previous.closed, 4003);
}

test('same-turn capability completions cannot split engine and session state', async t => {
  await t.test('candidate resolution queues first', { timeout: 10_000 }, async t => simultaneousCapabilityRefreshes(t, [0, 1]));
  await t.test('current-session refresh queues first', { timeout: 10_000 }, async t => simultaneousCapabilityRefreshes(t, [1, 0]));
});

test('a connection closed during refresh leaves the existing session available', { timeout: 10_000 }, async t => {
  const engine = fakeEngine();
  const { server, url } = await start(t, { engine });
  const previous = await connectDevice(url);
  const session = server.getSession(DEFAULT_DEVICE_KEY.device);
  const entered = deferred(), release = deferred();
  engine.prepareCapabilities = () => { entered.resolve(); return release.promise; };
  engine.applyCapabilities = () => {};
  t.after(() => release.resolve());
  const abandoned = await connectDevice(url, { awaitAuth: false });
  t.after(() => { previous.close(); abandoned.close(); });
  await entered.promise;
  abandoned.close();
  await abandoned.closed;
  release.resolve();
  assert.equal(session.closed, false);
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device), session);
  previous.send({ t: 'ping', ts: 100 });
  await previous.waitFor(event => event.t === 'pong' && event.ts === 100);
});

test('a replaced socket closing late cannot roll back the last speech generation', { timeout: 10_000 }, async t => {
  const { server, url } = await start(t);
  const first = await connectDevice(url);
  const old = server.getSession(DEFAULT_DEVICE_KEY.device);
  // Delay transport close until after the replacement disconnects. The session
  // itself still closes normally; its later socket event must not own lastGen.
  const close = old.ws.close.bind(old.ws);
  old.ws.close = () => {};
  t.after(() => close());
  old.gen = 10;
  const second = await connectDevice(url);
  const current = server.getSession(DEFAULT_DEVICE_KEY.device);
  assert.equal(current.gen, 10);
  current.gen = 11;
  second.close();
  await second.closed;
  close();
  assert.equal(await first.closed, 1005);
  const third = await connectDevice(url);
  t.after(() => third.close());
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).gen, 11,
    'the closed replacement owns the saved generation, not its older socket');
});
