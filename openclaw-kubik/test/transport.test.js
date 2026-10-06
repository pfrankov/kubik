// Protocol v5 transports end to end: real TLS sockets, real UDP and a real HTTP "Gateway" on loopback.
import assert from 'node:assert/strict';
import { createSocket } from 'node:dgram';
import { chmodSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { connect as connectTcp, createServer as createTcpServer } from 'node:net';
import { join } from 'node:path';
import { connect as connectTls, createServer as createTlsServer } from 'node:tls';
import test from 'node:test';
import WebSocket from 'ws';
import { discoverServer } from '../../tools/usb-bridge/lan-discovery.mjs';
import { channelPlugin } from '../src/channel.js';
import { routeStatus } from '../src/gateway-route.js';
import { DISCOVERY_REQUEST } from '../src/protocol.js';
import { transportStatus } from '../src/monitor.js';
import { IDENTITY_FILE, loadTlsIdentity } from '../src/tls-identity.js';
import { account, connectDevice, sleep } from './helpers.js';
import { fakeGateway, start, stateDir } from './server-fixture.js';

const outcome = (device) => device.waitFor((event) => ['welcome', 'pair', 'error'].includes(event.t), 5000, true);

/** A relaying MITM: terminates the device's TLS with its own key and forwards the bytes to the real server. */
async function relay(t, { port, tls }) {
  const identity = loadTlsIdentity(stateDir(t));
  const server = createTlsServer({ key: identity.key, cert: identity.cert }, (client) => {
    const upstream = tls ? connectTls({ host: '127.0.0.1', port, rejectUnauthorized: false }) : connectTcp(port, '127.0.0.1');
    const kill = () => { client.destroy(); upstream.destroy(); };
    for (const socket of [client, upstream]) { socket.on('error', kill); socket.on('close', kill); }
    client.pipe(upstream).pipe(client);
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  return { url: `wss://127.0.0.1:${server.address().port}/kubik/v1`, spki: identity.spkiHash };
}

/** Sends `count` discovery datagrams (or `payload`) and collects the replies for `waitMs`. */
async function discover(port, { count = 1, payload = DISCOVERY_REQUEST, waitMs = 200 } = {}) {
  const socket = createSocket('udp4');
  const replies = [];
  socket.on('message', (message, peer) => replies.push({ reply: JSON.parse(message), from: peer.address }));
  await new Promise((resolve) => socket.bind(0, '127.0.0.1', resolve));
  for (let i = 0; i < count; i++) socket.send(payload, port, '127.0.0.1');
  await sleep(waitMs);
  socket.close();
  return replies;
}

test('LAN: TLS with the persistent self-signed key; a device signing the SPKI it saw is welcomed', async (t) => {
  const { lanUrl, lan, logs } = await start(t);
  assert.match(lan.spki, /^[0-9a-f]{64}$/);
  const device = await connectDevice(lanUrl, { bind: 'seen' });
  assert.equal(device.peerSpki, lan.spki, 'the device sees exactly the key the status reports');
  assert.equal((await outcome(device)).t, 'welcome');
  assert.ok(logs.some((line) => /connected from 127\.0\.0\.1 via lan/.test(line)));
  device.close();
});

test('transport binding: LAN requires its own SPKI, the Gateway route requires ca:<its host>; any other value is 4001', async (t) => {
  const { url, lanUrl, lan } = await start(t);
  const refused = [[lanUrl, 'ca'], [lanUrl, 'ca:127.0.0.1'], [lanUrl, 'none'], [lanUrl, '0'.repeat(64)], [url, lan.spki], [url, 'none'], [url, 'ca:localhost']];
  for (const [target, bind] of refused) {
    for (const fw of ['0.6.0', '0.6.1']) { // the rejected hostless plain `ca` is a gateway-route rule; the LAN never takes it
      const device = await connectDevice(target, { bind, fw });
      assert.equal((await outcome(device))?.code, 'unauthorized', `${target} with ${bind} (fw ${fw})`);
      assert.equal(await device.closed, 4001);
    }
  }
  const good = await connectDevice(url, { bind: 'ca:127.0.0.1' });
  assert.equal((await outcome(good)).t, 'welcome');
  good.close();
});

test('relay MITM: a device that trusts the relay signs the relay key, so the real server refuses it (4001)', async (t) => {
  const { lan, gateway, logs } = await start(t);
  const toLan = await relay(t, { port: lan.port, tls: true });
  const toRoute = await relay(t, { port: gateway.http.address().port, tls: false });
  for (const mitm of [toLan, toRoute]) {
    const device = await connectDevice(mitm.url, { bind: 'seen' });
    assert.equal(device.peerSpki, mitm.spki);
    assert.notEqual(device.peerSpki, lan.spki);
    assert.equal((await outcome(device))?.code, 'unauthorized');
    assert.equal(await device.closed, 4001);
  }
  assert.ok(logs.some((line) => /transport binding mismatch/.test(line)));
});

test('listen.public: a non-private peer gets TLS and the v5 handshake but still no discovery reply; status says public', async (t) => {
  const { lanUrl, lan, statuses } = await start(t, { listen: { public: true }, lan: { isPrivatePeer: () => false } });
  const device = await connectDevice(lanUrl, { bind: 'seen' });
  assert.equal(device.peerSpki, lan.spki);
  assert.equal((await outcome(device)).t, 'welcome');
  device.close();
  assert.deepEqual(await discover(lan.discovery), []);
  assert.match(statuses.findLast((status) => status.lifecycle === 'ready').mode, new RegExp(`^lan .*:${lan.port} public spki:`));
});

test('private-peer filter (listen.public false, the default): a non-private peer gets neither TLS nor a discovery reply', async (t) => {
  const { lanUrl, lan, statuses } = await start(t, { lan: { isPrivatePeer: () => false } });
  await assert.rejects(connectDevice(lanUrl, { bind: 'seen' }));
  assert.deepEqual(await discover(lan.discovery), []);
  assert.doesNotMatch(statuses.findLast((status) => status.lifecycle === 'ready').mode, / public /);
});

test('public peers are still bounded before TLS: at most `perPeer` sockets from one address', async (t) => {
  const { lan } = await start(t, { listen: { public: true }, lan: { isPrivatePeer: () => false, limits: { perPeer: 2 } } });
  const sockets = [];
  for (let i = 0; i < 3; i++) sockets.push(connectTcp(lan.port, '127.0.0.1').on('error', () => {}));
  t.after(() => sockets.forEach((socket) => socket.destroy()));
  await sleep(300);
  assert.equal(sockets.filter((socket) => socket.destroyed).length, 1);
});

test('discovery: the exact request gets one unicast JSON reply with the TLS port, at most 10 per second', async (t) => {
  const { lan } = await start(t);
  const [first, ...rest] = await discover(lan.discovery);
  assert.deepEqual(rest, []);
  assert.deepEqual(first, { reply: { t: 'kubik', v: 5, port: lan.port }, from: '127.0.0.1' });
  assert.ok(Buffer.byteLength(JSON.stringify(first.reply)) <= 128);
  assert.equal(await discoverServer({ address: '127.0.0.1', port: lan.discovery }), `wss://127.0.0.1:${lan.port}/kubik/v1`);
  for (const payload of ['kubik-discover-v3', `${DISCOVERY_REQUEST}\n`, 'x'.repeat(2000)]) {
    assert.deepEqual(await discover(lan.discovery, { payload }), [], payload.slice(0, 20));
  }
  await sleep(1000 - (Date.now() % 1000) + 30); // one burst inside one second
  assert.equal((await discover(lan.discovery, { count: 25, waitMs: 300 })).length, 10);
});

test('the TLS key survives restarts (pinned devices keep working); a corrupt file is replaced, not fatal', async (t) => {
  const dir = stateDir(t);
  const first = await start(t, { lan: { stateDir: dir } });
  const pin = first.lan.spki;
  assert.equal(statSync(join(dir, IDENTITY_FILE)).mode & 0o777, 0o600);
  const again = await start(t, { lan: { stateDir: dir } });
  assert.equal(again.lan.spki, pin);
  const pinned = await connectDevice(again.lanUrl, { bind: 'seen' });
  assert.equal(pinned.peerSpki, pin, 'the pinned key is still served');
  assert.equal((await outcome(pinned)).t, 'welcome');
  pinned.close();

  writeFileSync(join(dir, IDENTITY_FILE), '{"version":1,"key":"broken"');
  chmodSync(join(dir, IDENTITY_FILE), 0o600);
  const recovered = await start(t, { lan: { stateDir: dir } });
  assert.notEqual(recovered.lan.spki, pin);
  assert.ok(recovered.logs.some((line) => /unusable/.test(line)));
  assert.ok(readdirSync(dir).some((name) => name.startsWith(`${IDENTITY_FILE}.corrupt-`)));
  const device = await connectDevice(recovered.lanUrl, { bind: 'seen' });
  assert.equal((await outcome(device)).t, 'welcome');
  device.close();
});

test('an unreadable key file is never replaced: LAN stays off with the error, the key is kept', { skip: process.getuid?.() === 0 }, async (t) => {
  const dir = stateDir(t);
  const pin = loadTlsIdentity(dir).spkiHash;
  chmodSync(join(dir, IDENTITY_FILE), 0o000);
  let statuses;
  try { ({ statuses } = await start(t, { lan: { stateDir: dir } })); } finally { chmodSync(join(dir, IDENTITY_FILE), 0o600); }
  assert.match(statuses.findLast((status) => status.lifecycle === 'ready').lastError, /EACCES/);
  assert.equal(loadTlsIdentity(dir).spkiHash, pin);
  assert.ok(!readdirSync(dir).some((name) => name.includes('.corrupt-')));
});

test('LAN listener bounds raw connections per peer before TLS (the pre-session limits start after the upgrade)', async (t) => {
  const { lan, server } = await start(t);
  const open = (socket) => new Promise((resolve) => {
    socket.once('secureConnect', () => resolve('open'));
    socket.once('close', () => resolve('closed'));
    socket.on('error', () => {});
  });
  const sockets = Array.from({ length: 9 }, () => connectTls({ host: '127.0.0.1', port: lan.port, rejectUnauthorized: false }));
  try {
    const results = await Promise.all(sockets.map(open));
    assert.equal(results.filter((result) => result === 'open').length, 8);
    sockets[0].destroy();
    await sleep(50);
    sockets.push(connectTls({ host: '127.0.0.1', port: lan.port, rejectUnauthorized: false }));
    assert.equal(await open(sockets.at(-1)), 'open', 'a closed connection frees its slot');
  } finally {
    const stopped = Promise.all(sockets.map((socket) => new Promise((resolve) => (socket.destroyed ? resolve() : socket.once('close', resolve)))));
    await server.stop();
    await stopped; // stop() closes idle TLS connections that never sent a request
  }
});

test('a busy LAN port is reported but not fatal: the Gateway route still serves devices', async (t) => {
  const busy = createTcpServer();
  await new Promise((resolve) => busy.listen(0, '127.0.0.1', resolve));
  t.after(() => busy.close());
  const { url, statuses } = await start(t, { lanPort: busy.address().port });
  const ready = statuses.findLast((status) => status.lifecycle === 'ready');
  assert.match(ready.lastError, /LAN listener is unavailable: port \d+ is already in use/);
  assert.match(ready.mode, /^lan unavailable .*; route \/kubik\/v1$/);
  const device = await connectDevice(url);
  assert.equal((await outcome(device)).t, 'welcome');
  device.close();
});

test('Gateway route: plain GET is 426, no running server is 503, registration is visible', async (t) => {
  const { gateway } = await start(t);
  const response = await fetch(gateway.url.replace('ws:', 'http:'));
  assert.equal(response.status, 426);
  assert.deepEqual(routeStatus(), { registered: true, path: '/kubik/v1' });
  const idle = await fakeGateway(t, () => undefined);
  const ws = new WebSocket(idle.url);
  ws.on('error', () => {});
  const [, rejected] = await new Promise((resolve) => ws.once('unexpected-response', (...args) => resolve(args)));
  assert.equal(rejected.statusCode, 503);
  rejected.destroy();
});

test('status: channels status shows LAN addresses, port, SPKI prefix and the route; probe carries the details', async (t) => {
  const { lan, statuses } = await start(t);
  const ready = statuses.findLast((status) => status.lifecycle === 'ready');
  assert.match(ready.mode, new RegExp(`^lan .*:${lan.port} spki:${lan.spki.slice(0, 16)}; route /kubik/v1$`));
  assert.equal(transportStatus().mode, ready.mode);
  const acc = account();
  const snapshot = channelPlugin.status.buildAccountSnapshot({ account: acc, runtime: { running: true } });
  assert.equal(snapshot.mode, ready.mode);
  const probe = await channelPlugin.status.probeAccount({ account: acc, timeoutMs: 200 });
  assert.equal(probe.lan.spki, lan.spki);
  assert.equal(probe.lan.port, lan.port);
  assert.ok(Array.isArray(probe.lan.addresses));
  assert.equal(probe.gatewayRoute.registered, true);
});
