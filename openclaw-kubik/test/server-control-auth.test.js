import assert from 'node:assert/strict';
import test from 'node:test';
import { connectDevice, deviceKey, fakeEngine, fakePairing } from './helpers.js';
import { start } from './server-fixture.js';

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

test('revocation during capability preparation rejects the candidate and preserves the approved live session', { timeout: 10_000 }, async (t) => {
  const device = 'kubik-b6c634';
  const oldKey = deviceKey(device);
  const candidateKey = deviceKey(device);
  const pairing = fakePairing({ allowed: [oldKey.entry, candidateKey.entry] });
  const engine = fakeEngine();
  const enteredPreparation = deferred();
  const candidatePreparation = deferred();
  const applied = [];
  let preparations = 0;
  engine.prepareCapabilities = () => {
    if (++preparations === 1) return Promise.resolve({ key: 'approved' });
    enteredPreparation.resolve();
    return candidatePreparation.promise;
  };
  engine.applyCapabilities = (snapshot) => applied.push(snapshot);

  const { server, url } = await start(t, { engine, pairing, serverOptions: { pairingPollMs: 60_000 } });
  const current = await connectDevice(url, { hello: oldKey.hello(), auth: (nonce) => oldKey.sign(nonce) });
  t.after(() => current.close());
  const currentSession = server.getSession(device);
  assert.deepEqual(applied, [{ key: 'approved' }]);

  const candidate = await connectDevice(url, { hello: candidateKey.hello(), auth: (nonce) => candidateKey.sign(nonce), awaitAuth: false });
  t.after(() => candidate.close());
  await enteredPreparation.promise;
  pairing.state.allowed = [oldKey.entry];
  candidatePreparation.resolve({ key: 'revoked-candidate' });

  assert.equal(await candidate.closed, 4001);
  assert.equal(candidate.events.some((event) => event.t === 'welcome'), false);
  assert.equal(server.getSession(device), currentSession, 'a denied reconnect must not replace the authorized session');
  assert.deepEqual(applied, [{ key: 'approved' }], 'a denied candidate cannot apply its prepared capabilities');
  current.send({ t: 'ping', ts: 71 });
  await current.waitFor((event) => event.t === 'pong' && event.ts === 71);
});

test('approved control requests work, and revocation blocks live agent controls before they run', { timeout: 10_000 }, async (t) => {
  for (const [name, message, expected] of [
    ['agent options', { t: 'agent_options', target: 'agent', cursor: 0 }, { options: 1, selections: 0 }],
    ['agent model change', { t: 'agent_model', target: 'agent', cursor: 0, id: 'provider/model' }, { options: 1, selections: 1 }],
  ]) {
    await t.test(name, async (t) => {
      const id = deviceKey();
      const pairing = fakePairing({ allowed: [id.entry] });
      const calls = { options: 0, selections: [] };
      const agentControl = {
        options: async () => { calls.options++; return { model: 'provider/model', models: [{ id: 'provider/model', label: 'Model' }] }; },
        selectModel: async (_device, model) => { calls.selections.push(model); },
      };
      const { url } = await start(t, { pairing, serverOptions: { pairingPollMs: 60_000, agentControl } });
      const device = await connectDevice(url, { hello: id.hello(), auth: (nonce) => id.sign(nonce) });
      t.after(() => device.close());

      device.send({ ...message, rid: 1 });
      await device.waitFor((event) => event.t === 'agent_options' && event.rid === 1);
      assert.deepEqual({ options: calls.options, selections: calls.selections.length }, expected);

      pairing.state.allowed = [];
      device.send({ ...message, rid: 2 });
      assert.equal(await device.closed, 4001);
      assert.deepEqual({ options: calls.options, selections: calls.selections.length }, expected,
        'revoked control frames are rejected before querying or changing agent settings');
    });
  }
});

test('approved device state and cancellation work, and revocation blocks both live controls', { timeout: 10_000 }, async (t) => {
  for (const [name, message, observe, expected] of [
    ['cancel', { t: 'cancel' }, (engine) => engine.calls.cancel, 1],
    ['device volume', { t: 'device_state', volume: 37 }, (_engine, session) => session.volume, 37],
  ]) {
    await t.test(name, async (t) => {
      const id = deviceKey();
      const pairing = fakePairing({ allowed: [id.entry] });
      const engine = fakeEngine();
      const { server, url } = await start(t, { pairing, engine, serverOptions: { pairingPollMs: 60_000 } });
      const device = await connectDevice(url, { hello: id.hello(), auth: (nonce) => id.sign(nonce) });
      t.after(() => device.close());
      const session = server.getSession(id.device);
      const handled = [];
      const handleMessage = session.handleMessage.bind(session);
      session.handleMessage = (value) => { handled.push(value.t); return handleMessage(value); };

      device.send(message);
      for (let attempt = 0; attempt < 100 && observe(engine, session) !== expected; attempt++) {
        await new Promise((resolve) => setImmediate(resolve));
      }
      assert.equal(observe(engine, session), expected, 'an approved device control should take effect');

      pairing.state.allowed = [];
      device.send(name === 'device volume' ? { ...message, volume: 12 } : message);
      assert.equal(await device.closed, 4001);
      assert.deepEqual(handled, [message.t], 'revocation must block the control before it reaches the session handler');
      if (name === 'device volume') assert.equal(session.volume, expected, 'revoked state updates must not mutate the live session');
    });
  }
});

test('the final admission allow-list read cannot publish a closed, superseded or failed candidate', async (t) => {
  for (const outcome of ['superseded', 'closed', 'store failure']) {
    await t.test(outcome, { timeout: 10_000 }, async (t) => {
      const id = deviceKey();
      const pairing = fakePairing({ allowed: [id.entry] });
      const entered = deferred(), checked = deferred();
      t.after(() => checked.resolve([id.entry]));
      const allowed = pairing.api.readAllowFromStore;
      let holdNextRead = false;
      pairing.api.readAllowFromStore = (params) => {
        if (!holdNextRead) return allowed(params);
        holdNextRead = false;
        entered.resolve();
        return checked.promise;
      };
      const engine = fakeEngine();
      let preparations = 0;
      const applied = [];
      engine.prepareCapabilities = () => {
        const snapshot = ++preparations;
        if (snapshot === 2) holdNextRead = true;
        return snapshot;
      };
      engine.applyCapabilities = (snapshot) => applied.push(snapshot);
      const { server, url } = await start(t, { pairing, engine, serverOptions: { pairingPollMs: 60_000 } });
      const options = { hello: id.hello(), auth: (nonce) => id.sign(nonce) };
      const current = await connectDevice(url, options);
      t.after(() => current.close());
      let expected = server.getSession(id.device);
      const candidate = await connectDevice(url, { ...options, awaitAuth: false });
      t.after(() => candidate.close());
      await entered.promise;

      if (outcome === 'superseded') {
        const newer = await connectDevice(url, options);
        t.after(() => newer.close());
        expected = server.getSession(id.device);
      } else if (outcome === 'closed') {
        candidate.close();
        await candidate.closed;
      }
      if (outcome === 'store failure') checked.reject(new Error('allow-list unavailable'));
      else checked.resolve([id.entry]);
      if (outcome !== 'closed') assert.equal(await candidate.closed, outcome === 'superseded' ? 4003 : 1011);
      await new Promise((resolve) => setImmediate(resolve));

      assert.equal(server.getSession(id.device), expected);
      assert.deepEqual(applied, outcome === 'superseded' ? [1, 3] : [1]);
      assert.equal(candidate.events.some((event) => event.t === 'welcome'), false);
    });
  }
});
