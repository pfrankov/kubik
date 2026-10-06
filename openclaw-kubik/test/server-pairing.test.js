import assert from 'node:assert/strict';
import test from 'node:test';
import { channelPlugin } from '../src/channel.js';
import { connectDevice, DEVICE, deviceKey, fakeEngine, fakePairing, sleep } from './helpers.js';
import { start, talk } from './server-fixture.js';

// ---- protocol v5: device keys, challenge–response and pairing ---------------------------------------------

/** Connects a device that answers the challenge with its own key (or `sig(nonce)`). */
const connectKeyed = (url, id, { sig, hello } = {}) => connectDevice(url, { hello: hello ?? id.hello(), auth: sig ?? ((nonce) => id.sign(nonce)) });

test('removing an approved key closes the socket before an immediate voice turn is dispatched', { timeout: 5000 }, async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const engine = fakeEngine();
  const { url, server, seen } = await start(t, { pairing, engine, serverOptions: { pairingPollMs: 60_000 } });
  const device = await connectKeyed(url, id);
  const reads = pairing.state.reads;

  pairing.state.allowed = [];
  talk(device, 1, 100);

  assert.equal(await device.closed, 4001);
  assert.equal(pairing.state.reads, reads + 1, 'the new turn performs a fresh allow-list read');
  assert.equal(engine.calls.begin, 0, 'revoked audio never reaches the voice engine');
  assert.equal(engine.calls.commit, 0);
  assert.equal(seen.dispatches.length, 0, 'revoked audio never reaches OpenClaw');
  assert.ok(!server.getSession(DEVICE) || server.getSession(DEVICE).closed);
});

test('revocation during recording blocks turn release before transcription starts', { timeout: 5000 }, async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const engine = fakeEngine();
  let began;
  const recording = new Promise((resolve) => { began = resolve; });
  const beginTurn = engine.beginTurn.bind(engine);
  engine.beginTurn = () => { beginTurn(); began(); };
  const { url, seen } = await start(t, { pairing, engine, serverOptions: { pairingPollMs: 60_000 } });
  const device = await connectKeyed(url, id);

  device.send({ t: 'ptt', on: true, turn: 1 });
  await recording;
  pairing.state.allowed = [];
  device.send({ t: 'ptt', on: false, turn: 1 });

  assert.equal(await device.closed, 4001);
  assert.equal(engine.calls.commit, 0);
  assert.equal(seen.dispatches.length, 0);
});

test('a notification after allow-list removal fails closed and closes the active socket', { timeout: 5000 }, async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const { url, server } = await start(t, { pairing, serverOptions: { pairingPollMs: 60_000 } });
  const device = await connectKeyed(url, id);
  const session = server.getSession(DEVICE);

  pairing.state.allowed = [];
  await assert.rejects(session.notify('Напоминание'), /device disconnected/);

  assert.equal(await device.closed, 4001);
  assert.equal(device.events.some((e) => e.t === 'speak'), false);
});

test('an allow-list refresh closes an idle revoked session', { timeout: 5000 }, async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const { url, server } = await start(t, { pairing, serverOptions: { pairingPollMs: 60_000 } });
  const device = await connectKeyed(url, id);

  pairing.state.allowed = [];
  await server.checkPairings();

  assert.equal(await device.closed, 4001);
  assert.ok(!server.getSession(DEVICE) || server.getSession(DEVICE).closed);
});

test('a pairing-store read failure closes before voice reaches the engine', { timeout: 5000 }, async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const engine = fakeEngine();
  const { url, seen } = await start(t, { pairing, engine, serverOptions: { pairingPollMs: 60_000 } });
  const device = await connectKeyed(url, id);
  pairing.api.readAllowFromStore = async () => { throw new Error('store unavailable'); };

  talk(device, 1, 100);

  assert.equal(await device.closed, 4001);
  assert.equal(engine.calls.begin, 0);
  assert.equal(seen.dispatches.length, 0);
});

test('an already-approved key gets challenge then welcome; the allow list is read via the pairing store', async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const { url, server, logs } = await start(t, { pairing });
  const device = await connectKeyed(url, id);
  await device.waitFor((e) => e.t === 'state' && e.s === 'idle');
  const [challenge, welcome] = device.events;
  assert.equal(challenge.t, 'challenge');
  assert.equal(Buffer.from(challenge.nonce, 'base64').length, 32);
  assert.deepEqual(welcome, { t: 'welcome', session: 's-1', progress: true, volume: 100 });
  assert.deepEqual(server.onlineDevices, [DEVICE]);
  assert.deepEqual(pairing.state.lastRead, { channel: 'kubik', accountId: 'default' });
  assert.equal(pairing.state.upserts.length, 0);
  assert.ok(logs.some((l) => /device kubik-b6c634 connected/.test(l)), logs.join('\n'));
  device.send({ t: 'ping', ts: 1 });
  await device.waitFor((e) => e.t === 'pong');
  device.send({ t: 'auth', sig: 'AAAA' });
  assert.equal(await device.closed, 4002, 'auth after welcome is a protocol error');
});

test('bad signature, a signature over another nonce and a non-auth frame are refused', async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const { url } = await start(t, { pairing });
  const other = deviceKey();
  const forged = await connectKeyed(url, id, { sig: (nonce) => other.sign(nonce, { key: id.key }) });
  assert.equal(await forged.closed, 4001);
  assert.deepEqual(forged.events.map((e) => e.t), ['challenge', 'error']);
  const replay = await connectKeyed(url, id, { sig: () => id.sign('b2xkIG5vbmNl') });
  assert.equal(await replay.closed, 4001);
  const garbage = await connectKeyed(url, id, { sig: () => 'bm90IGEgc2lnbmF0dXJl' });
  assert.equal(await garbage.closed, 4001);
  const skip = await connectDevice(url, { hello: id.hello() });
  await skip.waitFor((e) => e.t === 'challenge');
  skip.send({ t: 'ptt', on: true, turn: 1 });
  assert.equal(await skip.closed, 4002);
  const badKey = await connectDevice(url, { hello: id.hello({ key: Buffer.alloc(65, 4).toString('base64') }) });
  assert.equal(await badKey.closed, 4002);
  assert.equal(pairing.state.upserts.length, 0);
});

test('unknown key pairs on the same socket: pair code, ping while pending, approval -> welcome', async (t) => {
  const id = deviceKey();
  const pairing = fakePairing();
  const { url, server, logs } = await start(t, { pairing, serverOptions: { pairingPollMs: 30 } });
  const device = await connectKeyed(url, id);
  const pair = await device.waitFor((e) => e.t === 'pair');
  assert.deepEqual(pair, { t: 'pair', code: 'ABCD2342' });
  assert.deepEqual(pairing.state.upserts, [{ channel: 'kubik', id: id.entry, accountId: 'default',
    meta: { senderId: DEVICE, name: 'Кубик', fw: '0.6.1' } }]);
  assert.ok(logs.some((l) => l.includes(`new device ${DEVICE} wants to pair — approve with: openclaw pairing approve kubik ABCD2342`)), logs.join('\n'));
  device.send({ t: 'ping', ts: 7 });
  assert.deepEqual(await device.waitFor((e) => e.t === 'pong'), { t: 'pong', ts: 7 });
  await sleep(100);
  assert.deepEqual(server.onlineDevices, []);
  assert.ok(!device.events.some((e) => e.t === 'welcome'));
  pairing.approve(id.entry);
  assert.deepEqual(await device.waitFor((e) => e.t === 'welcome'), { t: 'welcome', session: 's-1', progress: true, volume: 100 });
  await device.waitFor((e) => e.t === 'state' && e.s === 'idle');
  assert.deepEqual(server.onlineDevices, [DEVICE]);
  assert.ok(logs.some((l) => l === `kubik: device ${DEVICE} approved`), logs.join('\n'));
  talk(device, 1);
  await device.waitFor((e) => e.t === 'speak_end');
});

test('checkPairings (notifyApproval) admits immediately; pending frames other than ping are refused', async (t) => {
  const id = deviceKey();
  const pairing = fakePairing();
  const { url } = await start(t, { pairing, serverOptions: { pairingPollMs: 60_000 } });
  const device = await connectKeyed(url, id);
  await device.waitFor((e) => e.t === 'pair');
  pairing.approve(id.entry);
  await channelPlugin.pairing.notifyApproval({ cfg: {}, id: id.entry });
  await device.waitFor((e) => e.t === 'welcome', 500);
  assert.equal(channelPlugin.pairing.idLabel, 'kubikDevice');
  assert.equal(channelPlugin.pairing.normalizeAllowEntry(` ${DEVICE.toUpperCase()}:AB `), `${DEVICE}:ab`);

  const other = deviceKey('kubik-other');
  const pending = await connectKeyed(url, other);
  await pending.waitFor((e) => e.t === 'pair');
  pending.send({ t: 'ptt', on: true, turn: 1 });
  assert.equal(await pending.closed, 4002);
});

test('empty code (store full) -> pair "" and 4005; pending timeout -> 4004 and the same code again', async (t) => {
  const pairing = fakePairing({ full: true });
  const { url } = await start(t, { pairing, serverOptions: { pairingTimeoutMs: 80, pairingPollMs: 20 } });
  const busy = await connectKeyed(url, deviceKey());
  assert.equal(await busy.closed, 4005);
  assert.deepEqual(busy.events.at(-1), { t: 'pair', code: '' });

  pairing.state.full = false;
  const id = deviceKey();
  const slow = await connectKeyed(url, id);
  const { code } = await slow.waitFor((e) => e.t === 'pair');
  assert.equal(await slow.closed, 4004);
  const again = await connectKeyed(url, id);
  assert.equal((await again.waitFor((e) => e.t === 'pair')).code, code);
  again.close();
});

test('device disabled in config is refused even with an approved key', async (t) => {
  const id = deviceKey();
  const pairing = fakePairing({ allowed: [id.entry] });
  const { url } = await start(t, { pairing, settings: { devices: { [DEVICE]: { enabled: false } } } });
  const device = await connectKeyed(url, id);
  assert.equal(await device.closed, 4001);
  assert.deepEqual(device.events.map((e) => e.t), ['challenge', 'error']);
  assert.equal(pairing.state.upserts.length, 0);
});

test('an approval binds the device id to its key', async (t) => {
  const approved = deviceKey('kubik-aaaaaa');
  const pairing = fakePairing({ allowed: [approved.entry] });
  const { url, server } = await start(t, { pairing });
  // Same key, different claimed id: not approved -> pairing request for the new id.
  const renamed = deviceKey(DEVICE);
  const sameKey = { ...renamed, key: approved.key, hello: () => ({ ...approved.hello(), device: DEVICE }),
    sign: (nonce) => approved.sign(nonce, { device: DEVICE }) };
  const impostor = await connectKeyed(url, sameKey);
  await impostor.waitFor((e) => e.t === 'pair');
  assert.equal(pairing.state.upserts[0].id, `${DEVICE}:${approved.fingerprint}`);
  // Same id, different key: not approved either.
  const otherKey = await connectKeyed(url, deviceKey('kubik-aaaaaa'));
  await otherKey.waitFor((e) => e.t === 'pair');
  assert.deepEqual(server.onlineDevices, []);
  const real = await connectKeyed(url, approved);
  await real.waitFor((e) => e.t === 'welcome');
  assert.deepEqual(server.onlineDevices, ['kubik-aaaaaa']);
});

test('at most 2 devices wait for pairing per address', async (t) => {
  const pairing = fakePairing();
  const { url } = await start(t, { pairing });
  const first = await connectKeyed(url, deviceKey('kubik-1'));
  const second = await connectKeyed(url, deviceKey('kubik-2'));
  await first.waitFor((e) => e.t === 'pair');
  await second.waitFor((e) => e.t === 'pair');
  const third = await connectKeyed(url, deviceKey('kubik-3'));
  assert.equal(await third.closed, 4005);
  assert.equal(pairing.state.upserts.length, 2);
  first.close();
  await first.closed;
  await sleep(20);
  const fourth = await connectKeyed(url, deviceKey('kubik-4'));
  await fourth.waitFor((e) => e.t === 'pair');
});

test('anonymous pre-auth sockets are globally bounded', async (t) => {
  const { url } = await start(t, { pairing: fakePairing() });
  const stalled = await Promise.all(Array.from({ length: 16 }, (_, i) => connectDevice(url, { hello: null,
    headers: { 'x-forwarded-for': `198.51.100.${i + 1}` } })));
  const refused = await connectDevice(url, { hello: null, headers: { 'x-forwarded-for': '198.51.100.99' } });
  assert.equal(await refused.closed, 4005);
  for (const device of stalled) device.close();
  await Promise.all(stalled.map((device) => device.closed));
});

test('one proxied client cannot occupy another client\'s handshake slots', async (t) => {
  const id = deviceKey();
  const { url } = await start(t, { pairing: fakePairing({ allowed: [id.entry] }) });
  const stalled = await Promise.all(Array.from({ length: 4 }, () => connectDevice(url, { hello: null,
    headers: { 'x-forwarded-for': '198.51.100.1' } })));
  const good = await connectDevice(url, { hello: id.hello(), auth: (nonce) => id.sign(nonce),
    headers: { 'x-forwarded-for': '198.51.100.2' } });
  assert.equal((await good.waitFor((e) => e.t === 'welcome')).t, 'welcome');
  for (const device of stalled) device.close();
  good.close();
  await Promise.all([...stalled.map((device) => device.closed), good.closed]);
});

test('authenticated sockets stay in bounded admission slots while the pairing store hangs', { timeout: 20000 }, async (t) => {
  const pairing = fakePairing();
  let readCount = 0;
  const readResolvers = [];
  const readWaiters = [];
  const waitForReads = (count) => readCount >= count ? Promise.resolve() : new Promise((resolve) => readWaiters.push({ count, resolve }));
  pairing.api.readAllowFromStore = async (params) => {
    pairing.state.reads++;
    pairing.state.lastRead = params;
    readCount++;
    for (const waiter of readWaiters.splice(0)) {
      if (readCount >= waiter.count) waiter.resolve(); else readWaiters.push(waiter);
    }
    return new Promise((resolve) => readResolvers.push(resolve));
  };
  const { url, server } = await start(t, { pairing });
  const held = [];
  const open = async (n, remote) => {
    const id = deviceKey(`kubik-${n.toString(16).padStart(6, '0')}`);
    return connectDevice(url, { hello: id.hello(), auth: (nonce) => id.sign(nonce), awaitAuth: false,
      headers: { 'x-forwarded-for': remote } });
  };
  const occupyRead = async (n, remote, expectedReads) => {
    const read = waitForReads(expectedReads);
    const device = await open(n, remote);
    held.push(device);
    await device.waitFor((event) => event.t === 'challenge');
    await read;
  };

  for (let i = 0; i < 4; i++) await occupyRead(i, '198.51.100.1', i + 1);
  const fifthFromOneAddress = await open(4, '198.51.100.1');
  assert.equal(await fifthFromOneAddress.closed, 4005);
  assert.equal(fifthFromOneAddress.events.some((event) => event.t === 'challenge'), false);
  assert.equal(readCount, 4);

  for (let remote = 2; remote <= 4; remote++) {
    for (let slot = 0; slot < 4; slot++) {
      const n = 5 + (remote - 2) * 4 + slot;
      await occupyRead(n, `198.51.100.${remote}`, readCount + 1);
    }
  }
  assert.equal(readCount, 16);
  const seventeenth = await open(18, '198.51.100.5');
  assert.equal(await seventeenth.closed, 4005);
  assert.equal(seventeenth.events.some((event) => event.t === 'challenge'), false);
  assert.equal(readCount, 16);

  for (const device of held) device.close();
  await Promise.all(held.map((device) => device.closed));
  await sleep(20); // let the server process every close and release its source slot
  for (const resolve of readResolvers.splice(0)) resolve([]);
  await sleep(20);
  assert.equal(pairing.state.upserts.length, 0, 'closed checking sockets do not create pairing requests after the read resolves');
  assert.deepEqual(server.onlineDevices, [], 'closed checking sockets do not become sessions after the read resolves');

  pairing.api.readAllowFromStore = async (params) => {
    pairing.state.reads++;
    pairing.state.lastRead = params;
    return [];
  };
  const recovered = await open(17, '198.51.100.1');
  await recovered.waitFor((event) => event.t === 'pair');
  recovered.close();
  await recovered.closed;
});

test('continuous bad-signature churn from one client leaves another client able to pair', async (t) => {
  const allowed = deviceKey();
  const attacker = deviceKey('kubik-attacker');
  const { url } = await start(t, { pairing: fakePairing({ allowed: [allowed.entry] }) });
  const churn = (async () => {
    for (let i = 0; i < 8; i++) {
      const bad = await connectDevice(url, { hello: attacker.hello(), auth: () => 'bm9wZQ==',
        headers: { 'x-forwarded-for': '198.51.100.1' } });
      assert.equal(await bad.closed, 4001);
    }
  })();
  await sleep(30);
  const good = await connectDevice(url, { hello: allowed.hello(), auth: (nonce) => allowed.sign(nonce),
    headers: { 'x-forwarded-for': '198.51.100.2' } });
  assert.equal((await good.waitFor((e) => e.t === 'welcome')).t, 'welcome');
  await churn;
  good.close();
  await good.closed;
});

test('spoofed hello with an approved public key does not lock out its owner', async (t) => {
  const id = deviceKey();
  const { url } = await start(t, { pairing: fakePairing({ allowed: [id.entry] }) });
  for (let i = 0; i < 10; i++) {
    const bad = await connectDevice(url, { hello: id.hello(), auth: () => 'bm9wZQ==', headers: { 'x-forwarded-for': '198.51.100.1' } });
    assert.equal(await bad.closed, 4001);
  }
  const good = await connectDevice(url, { hello: id.hello(), auth: (nonce) => id.sign(nonce), headers: { 'x-forwarded-for': '198.51.100.1' } });
  assert.equal((await good.waitFor((e) => e.t === 'welcome')).t, 'welcome');
  good.close();
  await good.closed;
});

test('without the OpenClaw pairing API is refused', async (t) => {
  const { url, logs } = await start(t, { pairing: null });
  const device = await connectKeyed(url, deviceKey());
  assert.equal(await device.closed, 4001);
  assert.ok(logs.some((l) => /pairing API is unavailable/.test(l)), logs.join('\n'));
  assert.ok(logs.some((l) => /pairing is unavailable/.test(l)), logs.join('\n'));
});

test('stalled pairing writes time out, release slots, and ignore late completion', { timeout: 12000 }, async (t) => {
  const releases = [];
  const pairing = fakePairing();
  const upsert = pairing.api.upsertPairingRequest;
  pairing.api.upsertPairingRequest = (...args) => new Promise((resolve) => releases.push(() => resolve(upsert(...args))));
  const { url, server } = await start(t, { pairing });
  const devices = await Promise.all([deviceKey(), deviceKey()].map((key) =>
    connectDevice(url, { hello: key.hello(), auth: (nonce) => key.sign(nonce), awaitAuth: false })));
  assert.deepEqual(await Promise.all(devices.map((d) => d.closed)), [1011, 1011]);
  assert.equal(releases.length, 2);
  for (const release of releases) release();
  await sleep(30);
  assert.deepEqual(server.onlineDevices, []);
  assert.ok(devices.every((d) => !d.events.some((e) => e.t === 'welcome' || e.t === 'pair')));
  pairing.api.upsertPairingRequest = upsert;
  const next = await connectKeyed(url, deviceKey());
  assert.ok((await next.waitFor((e) => e.t === 'pair')).code, 'same remote can pair after both slots time out');
});
