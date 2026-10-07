import assert from 'node:assert/strict';
import test from 'node:test';
import { connectDevice, DEFAULT_DEVICE_KEY, fakeEngine } from './helpers.js';
import { start } from './server-fixture.js';

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

test('concurrent capability refreshes replace the current connection atomically', { timeout: 10_000 }, async t => {
  const engine = fakeEngine();
  const { server, url } = await start(t, { engine });
  const previous = await connectDevice(url);
  const refreshes = [], ready = deferred();
  engine.refreshCapabilities = () => {
    const refresh = deferred();
    refreshes.push(refresh);
    if (refreshes.length === 2) ready.resolve();
    return refresh.promise;
  };
  t.after(() => { for (const refresh of refreshes) refresh.resolve(); });
  const first = await connectDevice(url, { awaitAuth: false });
  const second = await connectDevice(url, { awaitAuth: false });
  t.after(() => { previous.close(); first.close(); second.close(); });
  await ready.promise;
  refreshes[0].resolve();
  await first.waitFor(event => event.t === 'welcome');
  assert.equal(await previous.closed, 4003);
  refreshes[1].resolve();
  await second.waitFor(event => event.t === 'welcome');
  assert.equal(await first.closed, 4003, 'the connection admitted during refresh must also be replaced');
  assert.equal(server.getSession(DEFAULT_DEVICE_KEY.device).sessionId,
    second.events.find(event => event.t === 'welcome').session);
  second.send({ t: 'ping', ts: 99 });
  await second.waitFor(event => event.t === 'pong' && event.ts === 99);
  assert.deepEqual(server.onlineDevices, [DEFAULT_DEVICE_KEY.device]);
});

test('a connection closed during refresh leaves the existing session available', { timeout: 10_000 }, async t => {
  const engine = fakeEngine();
  const { server, url } = await start(t, { engine });
  const previous = await connectDevice(url);
  const session = server.getSession(DEFAULT_DEVICE_KEY.device);
  const entered = deferred(), release = deferred();
  engine.refreshCapabilities = () => { entered.resolve(); return release.promise; };
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
