import assert from 'node:assert/strict';
import fs from 'node:fs';
import { chmodSync, readFileSync, statSync } from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { join } from 'node:path';
import test from 'node:test';
import { connectDevice, DEFAULT_DEVICE_KEY, DEVICE, deviceKey, fakeEngine, fakePairing, sleep } from './helpers.js';
import { NotificationQueue } from '../src/notification-queue.js';
import { start, stateDir } from './server-fixture.js';

const contents = (path) => JSON.parse(readFileSync(path, 'utf8')).entries;

async function mixedNotification(t, notificationPath) {
  const engine = fakeEngine(), speak = engine.speak.bind(engine);
  let calls = 0;
  engine.speak = async (...args) => {
    if (++calls === 1) return speak(...args);
    throw Error('later TTS segment failed');
  };
  const { url, server } = await start(t, { engine,
    ...(notificationPath ? { serverOptions: { notificationPath } } : {}) });
  const device = await connectDevice(url, { autoPlayed: false, autoShown: false });
  t.after(() => device.close());
  const delivery = server.notify(DEVICE, '[[happy]] First sentence. [[sad]] Second sentence.');
  delivery.catch(() => {});
  return { server, device, delivery };
}

const acknowledgePlayback = (device, event) => device.send({ t: 'played', gen: event.gen,
  ms: Math.ceil(device.receivedMs(event.gen)) });

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

test('a queue persistence failure cannot keep a revoked live session open', { timeout: 10_000 }, async (t) => {
  for (const operation of ['pairing refresh', 'notification send']) {
    await t.test(operation, async (t) => {
      const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry] });
      const notificationPath = join(stateDir(t), 'notifications.json');
      const { url, server } = await start(t, { pairing, serverOptions: { notificationPath, pairingPollMs: 60_000 } });
      const device = await connectDevice(url, { autoPlayed: false });
      t.after(() => device.close());

      const delivery = server.notify(DEVICE, 'Неотправленное событие');
      delivery.catch(() => {});
      await device.waitFor((event) => event.t === 'speak_end');
      assert.equal(contents(notificationPath).length, 1, 'the durable notification remains pending until playback ACK');
      pairing.state.allowed = [];

      const failPersistence = t.mock.method(fs, 'fsyncSync', () => { throw new Error('injected queue sync failure'); });
      syncBuiltinESMExports();
      try {
        const operationPromise = operation === 'pairing refresh'
          ? server.checkPairings()
          : server.notify(DEVICE, 'This must not be accepted');
        await assert.rejects(operationPromise, /injected queue sync failure/);
        assert.equal(await device.closed, 4001, 'the revoked session is closed before queue persistence is attempted');
      } finally {
        failPersistence.mock.restore();
        syncBuiltinESMExports();
      }
    });
  }
});

test('an offline queue polls pairing revocation without sessions and stops after purge', { timeout: 5000 }, async (t) => {
  const pairing = fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry] });
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { server } = await start(t, { pairing, serverOptions: { notificationPath, pairingPollMs: 10 } });
  assert.equal((await server.notify(DEVICE, 'Событие, ожидающее устройство')).status, 'queued');
  assert.deepEqual(server.onlineDevices, []);
  const readsBeforeRevocation = pairing.state.reads;

  pairing.state.allowed = [];
  await sleep(60); // more than two configured pairing-poll intervals, with no live or pending socket
  assert.equal(contents(notificationPath).length, 0, 'the queue-only poll must remove content for a revoked key');
  const readsAfterPurge = pairing.state.reads;
  assert.ok(readsAfterPurge > readsBeforeRevocation);
  await sleep(30);
  assert.equal(pairing.state.reads, readsAfterPurge, 'the poll should stop after the queue drains');
});

test('a restored offline queue is permission-repaired and revocation-polled with LAN disabled', { timeout: 5000 }, async (t) => {
  const directory = stateDir(t);
  const notificationPath = join(directory, 'notifications.json');
  const queue = new NotificationQueue({ path: notificationPath });
  queue.enqueue(DEVICE, 'Событие со старого запуска', [DEFAULT_DEVICE_KEY.fingerprint]);
  chmodSync(directory, 0o755);
  chmodSync(notificationPath, 0o644);
  const pairing = fakePairing({ allowed: [] });

  const { server } = await start(t, { pairing, lan: { stateDir: directory }, listen: { enabled: false },
    serverOptions: { notificationPath, pairingPollMs: 10 } });

  if (process.platform !== 'win32') {
    assert.equal(statSync(directory).mode & 0o777, 0o700);
    assert.equal(statSync(notificationPath).mode & 0o777, 0o600);
  }
  await sleep(60); // restored content must trigger a poll even before any device connects
  assert.equal(contents(notificationPath).length, 0);
  assert.equal(server.lan?.off, true);
  const readsAfterPurge = pairing.state.reads;
  await sleep(30);
  assert.equal(pairing.state.reads, readsAfterPurge, 'the queue-only poll should stop once restored content is removed');
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

test('PCM emitted before a synthesis failure still requires playback and card acknowledgements', async (t) => {
  const engine = fakeEngine(), speak = engine.speak.bind(engine);
  engine.speak = async (...args) => { await speak(...args); throw Error('TTS failed after emitting PCM'); };
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { url, server } = await start(t, { engine, serverOptions: { notificationPath } });
  const device = await connectDevice(url, { autoPlayed: false, autoShown: false });
  t.after(() => device.close());
  const delivery = server.notify(DEVICE, 'The complete reply is on the screen.');
  let settled = false;
  delivery.then(() => { settled = true; }, () => { settled = true; });
  const end = await device.waitFor(event => event.t === 'speak_end');
  const card = await device.waitFor(event => event.t === 'text');
  assert.ok(device.receivedMs(end.gen) > 0);
  device.send({ t: 'shown', receipt: card.receipt });
  await sleep(30);
  assert.equal(settled, false, 'card receipt must not acknowledge unconfirmed partial audio');
  assert.equal(contents(notificationPath).length, 1);
  acknowledgePlayback(device, end);
  const result = await delivery;
  assert.equal(result.status, 'shown');
  assert.equal(result.spokenChars, 0);
  assert.ok(result.shownChars > 0);
  assert.equal(contents(notificationPath).length, 0);
});

test('mixed spoken and fallback text notification waits for both final acknowledgements', async (t) => {
  const { device, delivery } = await mixedNotification(t);
  let settled = false;
  delivery.then(() => { settled = true; }, () => { settled = true; });
  const end = await device.waitFor(event => event.t === 'speak_end');
  const card = await device.waitFor(event => event.t === 'text');
  acknowledgePlayback(device, end);
  await sleep(20);
  assert.equal(settled, false, 'played audio does not confirm the unacknowledged fallback card');
  device.send({ t: 'shown', receipt: card.receipt });
  const result = await delivery;
  assert.equal(result.status, 'played');
  assert.ok(result.spokenChars > 0 && result.shownChars > 0);
});

test('mixed notification accepts the final card ACK before playback completes', async (t) => {
  const { device, delivery } = await mixedNotification(t);
  let settled = false;
  delivery.then(() => { settled = true; }, () => { settled = true; });
  const end = await device.waitFor(event => event.t === 'speak_end');
  const card = await device.waitFor(event => event.t === 'text');
  device.send({ t: 'shown', receipt: card.receipt });
  await sleep(20);
  assert.equal(settled, false, 'the card ACK cannot replace the playback ACK');
  acknowledgePlayback(device, end);
  const result = await delivery;
  assert.equal(result.status, 'played');
  assert.ok(result.spokenChars > 0 && result.shownChars > 0);
});

test('a successful spoken notification with a [[show]] card waits for its shown ACK', async (t) => {
  const { url, server } = await start(t);
  const device = await connectDevice(url, { autoPlayed: false, autoShown: false });
  t.after(() => device.close());
  let settled = false;
  const delivery = server.notify(DEVICE, 'Spoken prefix. [[show]]Card text[[/show]]');
  delivery.then(() => { settled = true; }, () => { settled = true; });
  const end = await device.waitFor(event => event.t === 'speak_end');
  const card = await device.waitFor(event => event.t === 'text');
  acknowledgePlayback(device, end);
  await sleep(20);
  assert.equal(settled, false, 'successful TTS does not confirm the separate screen card');
  device.send({ t: 'shown', receipt: card.receipt });
  const result = await delivery;
  assert.equal(result.status, 'played');
  assert.ok(result.spokenChars > 0 && result.shownChars > 0);
  assert.equal(settled, true);
});

test('mixed notification stays durable when the fallback card ACK is missing', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { device, delivery } = await mixedNotification(t, notificationPath);
  const rejected = assert.rejects(delivery, error => /acknowledgement timed out/.test(error.message) && error.attempted);
  const end = await device.waitFor(event => event.t === 'speak_end');
  await device.waitFor(event => event.t === 'text');
  acknowledgePlayback(device, end);
  await rejected;
  assert.equal(contents(notificationPath).length, 1, 'a played prefix does not remove an unconfirmed fallback');
});

test('interrupting mixed delivery while waiting for the fallback ACK consumes it intentionally', async (t) => {
  const notificationPath = join(stateDir(t), 'notifications.json');
  const { device, delivery } = await mixedNotification(t, notificationPath);
  const end = await device.waitFor(event => event.t === 'speak_end');
  await device.waitFor(event => event.t === 'text');
  acknowledgePlayback(device, end);
  await sleep(20);
  device.send({ t: 'cancel' });
  const result = await delivery;
  assert.equal(result.status, 'interrupted');
  assert.equal(contents(notificationPath).length, 0, 'an explicit cancel consumes the notification');
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
