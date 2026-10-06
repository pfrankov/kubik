// Transport edge cases: wrong LAN path, disabled LAN, route registration failures, listener fallbacks and limits.
import assert from 'node:assert/strict';
import { createSocket } from 'node:dgram';
import { connect as connectTcp, Server } from 'node:net';
import test from 'node:test';
import WebSocket from 'ws';
import { registerDeviceRoute, routeStatus } from '../src/gateway-route.js';
import { CLOSE } from '../src/protocol.js';
import { LanListener, reachableIPv4Addresses, deviceServerAddresses } from '../src/lan.js';
import { transportStatus } from '../src/monitor.js';
import { loadTlsIdentity } from '../src/tls-identity.js';
import { connectDevice, DEFAULT_DEVICE_KEY, DEVICE } from './helpers.js';
import { start, stateDir } from './server-fixture.js';

const outcome = (device) => device.waitFor((event) => ['welcome', 'pair', 'error'].includes(event.t), 5000, true);

test('LAN: any path other than /kubik/v1 is a 404 on the real listener', async (t) => {
  const { lan } = await start(t);
  const ws = new WebSocket(`wss://127.0.0.1:${lan.port}/other`, { rejectUnauthorized: false });
  ws.on('error', () => {});
  const [, response] = await new Promise((resolve) => ws.once('unexpected-response', (...args) => resolve(args)));
  assert.equal(response.statusCode, 404);
  response.destroy();
});

test('listen.enabled false: no LAN listener, "lan off" in status, the Gateway route still serves devices', async (t) => {
  const { url, lanUrl, statuses } = await start(t, { listen: { enabled: false } });
  assert.equal(lanUrl, undefined);
  assert.match(transportStatus().mode, /^lan off; route \/kubik\/v1$/);
  assert.deepEqual(statuses.findLast((status) => status.lifecycle === 'ready').lan, { enabled: false });
  const device = await connectDevice(url);
  assert.equal((await outcome(device)).t, 'welcome');
  device.close();
});

test('registerDeviceRoute reports a host without plugin routes or a registration that throws', (t) => {
  t.after(() => registerDeviceRoute({ registerHttpRoute: () => {} }, () => undefined, () => undefined));
  assert.equal(registerDeviceRoute({}, () => undefined, () => undefined), false);
  assert.match(routeStatus().error, /no plugin HTTP routes/);
  assert.equal(registerDeviceRoute({ registerHttpRoute: () => { throw new Error('path taken'); } }, () => undefined, () => undefined), false);
  assert.equal(routeStatus().error, 'path taken');
  assert.equal(registerDeviceRoute({ registerHttpRoute: () => {} }, () => undefined, () => undefined), true);
  assert.deepEqual(routeStatus(), { registered: true, path: '/kubik/v1' });
});

test('the default listener falls back from :: to 0.0.0.0 without IPv6', async (t) => {
  const listen = Server.prototype.listen;
  const hosts = [];
  Server.prototype.listen = function patched(options, ...rest) {
    hosts.push(options?.host);
    if (options?.host === '::') { queueMicrotask(() => this.emit('error', Object.assign(new Error('no ipv6'), { code: 'EAFNOSUPPORT' }))); return this; }
    return listen.call(this, options, ...rest);
  };
  const lan = new LanListener({ identity: loadTlsIdentity(stateDir(t)), port: 0, discoveryHost: '127.0.0.1', discoveryPort: 0, onUpgrade: () => {} });
  t.after(() => { Server.prototype.listen = listen; return lan.stop(); });
  await lan.start();
  assert.deepEqual(hosts, ['::', '0.0.0.0']);
  assert.ok(lan.status.port > 0);
});

test('a busy discovery port is reported but not fatal: TLS keeps serving', async (t) => {
  const busy = createSocket('udp4');
  await new Promise((resolve) => busy.bind(0, '127.0.0.1', resolve));
  t.after(() => busy.close());
  const { lanUrl, lan, logs } = await start(t, { lan: { discoveryPort: busy.address().port } });
  assert.equal(lan.discoveryError, 'EADDRINUSE');
  assert.equal(lan.discovery, null);
  assert.ok(logs.some((line) => /discovery on UDP \d+ is unavailable/.test(line)));
  const device = await connectDevice(lanUrl, { bind: 'seen' });
  assert.equal((await outcome(device)).t, 'welcome');
  device.close();
});

test('LAN limits: maxConnections drops the excess and a silent TLS handshake is cut after its timeout', async (t) => {
  const { lan } = await start(t, { lan: { limits: { connections: 2, handshakeMs: 300 } } });
  const sockets = Array.from({ length: 3 }, () => connectTcp(lan.port, '127.0.0.1'));
  const closed = sockets.map((socket) => new Promise((resolve) => { socket.on('error', () => {}); socket.once('close', () => resolve(Date.now())); }));
  const began = Date.now();
  const first = await Promise.race(closed);
  assert.ok(first - began < 250, 'the third connection is refused at once');
  const rest = await Promise.all(closed);
  assert.ok(Math.max(...rest) - began < 2000, 'idle handshakes are cut after handshakeMs');
});

test('LAN never reads proxy headers: the peer is the socket address', async (t) => {
  const { lanUrl, logs } = await start(t);
  const device = await connectDevice(lanUrl, { bind: 'seen', headers: { 'x-forwarded-for': '198.51.100.9', 'cf-connecting-ip': '198.51.100.9', 'x-real-ip': '198.51.100.9' } });
  assert.equal((await outcome(device)).t, 'welcome');
  assert.ok(logs.some((line) => /connected from 127\.0\.0\.1 via lan/.test(line)));
  assert.ok(!logs.some((line) => line.includes('198.51.100.9')));
  device.close();
});

test('Gateway route: the client is the Gateway\'s resolved address, the socket address without one', async (t) => {
  const { url, logs } = await start(t);
  const proxied = await connectDevice(url, { headers: { 'x-forwarded-for': '198.51.100.7' } });
  assert.equal((await outcome(proxied)).t, 'welcome');
  proxied.close();
  await proxied.closed;
  const direct = await connectDevice(url);
  assert.equal((await outcome(direct)).t, 'welcome');
  direct.close();
  assert.ok(logs.some((line) => /connected from 198\.51\.100\.7 via gateway/.test(line)), logs.join('\n'));
  assert.ok(logs.some((line) => /connected from 127\.0\.0\.1 via gateway/.test(line)));
});

test('a device that never says hello is dropped after about 3 seconds', { timeout: 8000 }, async (t) => {
  const { url } = await start(t);
  const began = Date.now();
  const device = await connectDevice(url, { hello: null });
  assert.equal(await device.closed, CLOSE.PROTOCOL);
  const elapsed = Date.now() - began;
  assert.ok(elapsed >= 2500 && elapsed < 4500, `closed after ${elapsed} ms`);
});

test('the SenderId of a device can never equal a pairing allow-list entry', () => {
  // `openclaw pairing approve` records `kubik:<device>:<fingerprint>` (and owner bootstrap copies it); the inbound
  // SenderId is the bare device id. Device ids cannot contain ':', so the two never collide.
  const entry = `kubik:${DEFAULT_DEVICE_KEY.entry}`;
  assert.notEqual(DEVICE, entry);
  assert.ok(entry.startsWith(`kubik:${DEVICE}:`));
  assert.equal(DEFAULT_DEVICE_KEY.entry.split(':').length, 2);
});


test('device addresses follow the actual bind and remain individually copyable', () => {
  const interfaces = { home: [{family: 'IPv4', internal: false, address: '192.168.1.5'}],
    vpn: [{family: 'IPv4', internal: false, address: '100.100.1.2'}],
    public: [{family: 'IPv4', internal: false, address: '203.0.113.5'}],
    loopback: [{family: 'IPv4', internal: true, address: '127.0.0.1'}] };
  assert.deepEqual(reachableIPv4Addresses('127.0.0.1', interfaces), []);
  assert.deepEqual(reachableIPv4Addresses('192.168.1.5', interfaces), ['192.168.1.5']);
  assert.deepEqual(reachableIPv4Addresses('::', interfaces), ['100.100.1.2', '192.168.1.5']);
  assert.deepEqual(reachableIPv4Addresses('203.0.113.5', interfaces, true), ['203.0.113.5']);
  assert.deepEqual(reachableIPv4Addresses(undefined, interfaces), []);
  assert.deepEqual(deviceServerAddresses({addresses: ['192.168.1.5', '100.100.1.2'], port: 19000}),
    ['kubik://192.168.1.5:19000', 'kubik://100.100.1.2:19000']);
});
