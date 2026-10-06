import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import test from 'node:test';
import { connectDevice, DEFAULT_DEVICE_KEY, DEVICE, deviceKey, fakeEngine, fakePairing, sleep } from './helpers.js';
import { start, stateDir } from './server-fixture.js';

const contents = (path) => JSON.parse(readFileSync(path, 'utf8')).entries;

test('sleeping device notifications persist across host restart and play in FIFO order on authenticated reconnect', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const old = await start(t, { serverOptions: { notificationPath } });
  const first = await old.server.notify(DEVICE, 'Первое событие');
  const second = await old.server.notify(DEVICE, 'Второе событие');
  assert.equal(first.status, 'queued');
  assert.equal(first.durable, true);
  assert.equal(second.status, 'queued');
  assert.equal(contents(notificationPath).length, 2);
  await old.server.stop();
  const { url, engine } = await start(t, { serverOptions: { notificationPath } });
  const device = await connectDevice(url);
  await device.waitForNth((event) => event.t === 'speak_end', 2);
  await device.waitForNth((event) => event.t === 'state' && event.s === 'idle', 3);
  assert.deepEqual(engine.calls.speak, ['Первое событие.', 'Второе событие.']);
  assert.equal(contents(notificationPath).length, 0);
  device.close();
  await device.closed;
  const reconnect = await connectDevice(url);
  await sleep(30);
  assert.equal(reconnect.events.some((event) => event.t === 'speak'), false);
});

test('unapproved or forged reconnect cannot drain queued notifications; the authorized key can', async (t) => {
  const { url, server, engine } = await start(t);
  await server.notify(DEVICE, 'Секретное событие');
  const unknown = deviceKey();
  const pending = await connectDevice(url, { hello: unknown.hello(), auth: (nonce) => unknown.sign(nonce) });
  assert.ok(pending.events.some((event) => event.t === 'pair'));
  assert.equal(engine.calls.speak.length, 0);
  pending.close();
  await pending.closed;
  const forged = await connectDevice(url, { auth: () => 'AAAA' });
  assert.equal(await forged.closed, 4001);
  assert.equal(engine.calls.speak.length, 0);
  const approved = await connectDevice(url);
  await approved.waitFor((event) => event.t === 'speak_end');
  assert.equal(engine.calls.speak.length, 1);
});

test('revoked keys and fresh device identities do not inherit offline notifications', async (t) => {
  const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry] });
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { url, server, engine } = await start(t, { pairing, serverOptions: { notificationPath } });
  await server.notify(DEVICE, 'Для старого ключа');
  pairing.state.allowed = [];
  await server.checkPairings();
  assert.equal(contents(notificationPath).length, 0, 'explicit revocation refresh purges even without a live socket');
  await assert.rejects(server.notify(DEVICE, 'Нельзя отправить'), /not approved/);
  assert.equal(contents(notificationPath).length, 0);
  const replacement = deviceKey();
  pairing.approve(replacement.entry);
  const device = await connectDevice(url, { hello: replacement.hello(), auth: (nonce) => replacement.sign(nonce) });
  await sleep(30);
  assert.equal(engine.calls.speak.length, 0);
  assert.equal(device.events.some((event) => event.t === 'speak'), false);
});

test('notification reports played only after matching device acknowledgement', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { url, server } = await start(t, { serverOptions: { notificationPath } });
  const device = await connectDevice(url, { autoPlayed: false });
  let settled = false;
  const promise = server.notify(DEVICE, 'Подтверждение').finally(() => { settled = true; });
  const end = await device.waitFor((event) => event.t === 'speak_end');
  await sleep(20);
  assert.equal(settled, false, 'sending audio is not playback');
  device.send({ t: 'played', gen: end.gen + 1, ms: 20 });
  await sleep(20);
  assert.equal(settled, false, 'wrong generation does not acknowledge playback');
  device.send({ t: 'played', gen: end.gen, ms: 0 });
  await sleep(20);
  assert.equal(settled, false, 'a refused/empty audio ring is not playback');
  assert.equal(contents(notificationPath).length, 1);
  device.send({ t: 'played', gen: end.gen, ms: 1 });
  await sleep(20);
  assert.equal(settled, false, 'partial playback does not acknowledge the full stream');
  assert.equal(contents(notificationPath).length, 1);
  device.send({ t: 'played', gen: end.gen, ms: 20 });
  const result = await promise;
  assert.equal(result.status, 'played');
  assert.equal(contents(notificationPath).length, 0);
  assert.ok(result.spokenChars > 0);
});

test('missing played acknowledgement retains delivery and replays on reconnect', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { url, server, engine } = await start(t, { serverOptions: { notificationPath } });
  const device = await connectDevice(url, { autoPlayed: false });
  const delivery = server.notify(DEVICE, 'Без подтверждения');
  const rejected = assert.rejects(delivery, (error) => /acknowledgement timed out/.test(error.message) && error.attempted === true);
  await device.waitFor((event) => event.t === 'speak_end');
  assert.equal(contents(notificationPath).length, 1, 'unacknowledged notification remains durable');
  await rejected;
  device.close();
  await device.closed;
  const reconnect = await connectDevice(url);
  await sleep(30);
  await reconnect.waitFor((event) => event.t === 'speak_end');
  await sleep(30);
  assert.equal(engine.calls.speak.length, 2);
  assert.equal(contents(notificationPath).length, 0);
});

test('disconnect before acknowledgement replays the notification and retains FIFO', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const engine = fakeEngine({ speakDelayMs: 30 });
  const { url, server } = await start(t, { engine, serverOptions: { notificationPath } });
  const device = await connectDevice(url, { autoPlayed: false });
  const first = server.notify(DEVICE, 'Первое');
  const failure = assert.rejects(first, (error) => /disconnected/.test(error.message) && error.attempted === true);
  await device.waitFor((event) => event.t === 'speak_end');
  const later = [server.notify(DEVICE, 'Второе'), server.notify(DEVICE, 'Третье')];
  // Wait until both have entered the durable queue before losing the socket.
  for (let retries = 0; contents(notificationPath).length < 3 && retries < 20; retries++) await sleep(5);
  assert.equal(contents(notificationPath).length, 3);
  device.ws.terminate();
  await failure;
  const queued = await Promise.all(later);
  assert.deepEqual(queued.map((result) => result.status), ['queued', 'queued']);
  const reconnect = await connectDevice(url);
  await reconnect.waitForNth((event) => event.t === 'speak_end', 3);
  assert.deepEqual(engine.calls.speak, ['Первое.', 'Первое.', 'Второе.', 'Третье.']);
});

test('text-only notification reports shown instead of falsely claiming played', async (t) => {
  const engine = fakeEngine();
  engine.canSpeak = false;
  const { url, server } = await start(t, { engine });
  const device = await connectDevice(url);
  const result = await server.notify(DEVICE, 'Текст на экране');
  assert.equal(result.status, 'shown');
  assert.equal(result.spokenChars, 0);
  assert.ok(result.shownChars > 0);
  assert.equal(device.events.some((event) => event.t === 'speak'), false);
  await device.waitFor((event) => event.t === 'text');
});

test('voice provider failure with a screen delivers shown without waiting for a nonexistent audio acknowledgement', async (t) => {
  const { url, server } = await start(t, { engine: fakeEngine({ failSpeak: true }) });
  const device = await connectDevice(url, { autoPlayed: false });
  const result = await server.notify(DEVICE, 'Виден без голоса');
  assert.equal(result.status, 'shown');
  assert.equal(result.spokenChars, 0);
  assert.ok(result.shownChars > 0);
  await device.waitFor((event) => event.t === 'text');
});

test('connection failure before the first output retains a notification waiting behind recording', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { url, server, engine } = await start(t, { serverOptions: { notificationPath } });
  const device = await connectDevice(url);
  device.send({ t: 'ptt', on: true, turn: 1 });
  for (let retries = 0; !engine.calls.begin && retries < 20; retries++) await sleep(5);
  assert.equal(engine.calls.begin, 1);
  const pending = server.notify(DEVICE, 'После вопроса');
  await sleep(20);
  assert.equal(contents(notificationPath).length, 1, 'queued speech has not attempted output yet');
  assert.equal(engine.calls.speak.length, 0);
  device.ws.terminate();
  assert.equal((await pending).status, 'queued');
  assert.equal(contents(notificationPath).length, 1);
  const reconnect = await connectDevice(url);
  await reconnect.waitFor((event) => event.t === 'speak_end');
  assert.equal(engine.calls.speak.length, 1);
  await reconnect.waitForNth((event) => event.t === 'state' && event.s === 'idle', 2);
  assert.equal(contents(notificationPath).length, 0);
});

test('user interruption after notification audio starts reports interrupted and does not replay', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { url, server } = await start(t, { serverOptions: { notificationPath } });
  const device = await connectDevice(url, { autoPlayed: false });
  const pending = server.notify(DEVICE, 'Можно прервать');
  await device.waitFor((event) => event.t === 'speak_end');
  device.send({ t: 'cancel' });
  const result = await pending;
  assert.equal(result.status, 'interrupted');
  assert.equal(result.cancelled, true);
  assert.equal(contents(notificationPath).length, 0);
});


test('text delivery waits for the final matching receipt and survives disconnect before ACK', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const engine = fakeEngine(); engine.canSpeak = false;
  const { url, server } = await start(t, { engine, serverOptions: { notificationPath } });
  const device = await connectDevice(url, { autoShown: false });
  let settled = false;
  const pending = server.notify(DEVICE, 'Текст с подтверждением').finally(() => { settled = true; });
  const rejected = assert.rejects(pending, /disconnected/);
  const card = await device.waitFor((event) => event.t === 'text');
  device.send({ t: 'shown', receipt: card.receipt + 1 });
  await sleep(30);
  assert.equal(settled, false);
  assert.equal(contents(notificationPath).length, 1);
  device.ws.terminate(); await rejected;
  const reconnect = await connectDevice(url, { autoShown: false });
  const replay = await reconnect.waitFor((event) => event.t === 'text');
  assert.equal(contents(notificationPath).length, 1);
  reconnect.send({ t: 'shown', receipt: replay.receipt });
  for (let tries = 0; contents(notificationPath).length && tries < 20; tries++) await sleep(10);
  assert.equal(contents(notificationPath).length, 0);
});
