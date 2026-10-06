import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import WebSocket, { WebSocketServer } from 'ws';
import { createSocket } from 'node:dgram';
import { UsbRoute, F, frame, Deframer } from './bridge.mjs';
import { routedPayload, serverTarget, serverUrl } from './bridge-protocol.mjs';
import { discoverServer } from './lan-discovery.mjs';
import { DISCOVERY_REQUEST } from '../../openclaw-kubik/src/protocol.js';

class Clock {
  now = 0; next = 1; timers = new Map();
  set = (fn, delay) => { const id = this.next++; this.timers.set(id, { fn, at: this.now + delay }); return id; };
  clear = (id) => this.timers.delete(id);
  tick(ms) {
    const end = this.now + ms;
    for (;;) {
      const items = [...this.timers].filter(([, v]) => v.at <= end).sort((a, b) => a[1].at - b[1].at);
      if (!items.length) break;
      const [id, event] = items[0]; this.now = event.at; this.timers.delete(id); event.fn();
    }
    this.now = end;
  }
}
class Socket extends EventEmitter {
  readyState = WebSocket.CONNECTING; sent = []; dead = false; pings = 0;
  send(data, opts) { this.sent.push({ data: data.toString(), opts }); }
  terminate() { this.dead = true; this.readyState = WebSocket.CLOSED; }
  ping() { this.pings++; }
  open() { this.readyState = WebSocket.OPEN; this.emit('open'); }
  welcome() { this.emit('message', Buffer.from('{"t":"welcome","session":"s-1"}'), false); }
}
const hello = JSON.stringify({ t: 'hello', v: 2, device: 'kubik-test', key: 'BAAA', fw: 'test' });
function setup(enabled = true) {
  const clock = new Clock(), writes = [], sockets = [];
  const route = new UsbRoute({ server: 'ws://test/kubik/v1', enabled, epoch: 100,
    send: (type, data) => writes.push({ type, data }), socketFactory: () => { const s = new Socket(); sockets.push(s); return s; },
    setTimer: clock.set, clearTimer: clock.clear });
  route.attach();
  return { route, clock, writes, sockets, hello: () => route.deviceFrame(F.JSON, routedPayload(route.epoch, hello)) };
}

test('authenticated welcome alone makes USB ready and is delivered after epoch heartbeat', () => {
  const s = setup(); s.hello(); assert.equal(s.sockets.length, 1);
  s.sockets[0].open(); assert.equal(s.route.status, 'probing');
  assert.equal(s.sockets[0].sent.length, 1);
  s.sockets[0].welcome(); assert.equal(s.route.status, 'ready');
  const [hb, welcome] = s.writes.slice(-2);
  assert.deepEqual(JSON.parse(hb.data), { v: 2, status: 'ready', epoch: s.route.epoch });
  assert.equal(welcome.type, F.JSON); assert.equal(welcome.data.readUInt32LE(0), s.route.epoch);
  assert.equal(JSON.parse(welcome.data.subarray(4)).t, 'welcome');
  s.hello(); assert.equal(s.sockets.length, 1); assert.equal(s.sockets[0].sent.length, 1);
  s.route.detach();
});
test('unreachable upstream backs off; unframed JSON and stale epochs cannot open routes', () => {
  const s = setup(); const old = s.route.epoch;
  s.route.deviceFrame(F.JSON, Buffer.from(hello)); assert.equal(s.sockets.length, 0);
  s.hello(); s.sockets[0].emit('error', new Error('refused'));
  assert.equal(s.route.status, 'unavailable'); assert(s.sockets[0].dead);
  s.hello(); assert.equal(s.sockets.length, 1);
  s.clock.tick(2000); assert.equal(s.route.status, 'probing'); assert.notEqual(s.route.epoch, old);
  s.route.deviceFrame(F.JSON, routedPayload(old, hello)); assert.equal(s.sockets.length, 1);
  s.hello(); s.sockets[1].open(); s.clock.tick(8000);
  assert.equal(s.route.status, 'unavailable'); assert(s.sockets[1].dead);
  s.route.detach();
});
test('standby: no upstream, no timers and no status churn until the device asks for USB', () => {
  const s = setup(); const epoch = s.route.epoch;
  s.clock.tick(10 * 60 * 1000);
  assert.equal(s.sockets.length, 0); assert.equal(s.route.status, 'probing'); assert.equal(s.route.epoch, epoch);
  assert.equal(s.clock.timers.size, 0);
  s.hello(); assert.equal(s.sockets.length, 1); assert.equal(s.clock.timers.size, 1); // the 8 s authentication bound
  s.route.detach();
});
test('a failed upstream backs off once, then the bridge idles: no reconnect while the device is silent on USB', () => {
  const s = setup(); s.hello(); s.sockets[0].open(); s.clock.tick(8000); // no welcome: authentication times out
  assert.equal(s.route.status, 'unavailable');
  s.clock.tick(2000); assert.equal(s.route.status, 'probing'); // the retry only re-announces standby
  assert.equal(s.clock.timers.size, 0);
  s.clock.tick(10 * 60 * 1000); // the device is online over Wi-Fi and sends no hello
  assert.equal(s.sockets.length, 1); assert.equal(s.route.status, 'probing'); assert.equal(s.clock.timers.size, 0);
  s.route.detach();
});
test('failback: the server replacing the session (Wi-Fi is back) returns USB to standby without backoff', () => {
  const s = setup(); s.hello(); s.sockets[0].open(); s.sockets[0].welcome();
  const epoch = s.route.epoch;
  s.sockets[0].emit('close', 4003);
  assert.equal(s.route.status, 'probing'); assert.notEqual(s.route.epoch, epoch); assert.equal(s.route.retry, 0);
  assert.equal(s.clock.timers.size, 0); s.clock.tick(60000); assert.equal(s.sockets.length, 1);
  s.hello(); assert.equal(s.sockets.length, 2); // Wi-Fi lost again: the next hello reconnects at once
  s.route.detach();
});
test('disabled route keeps host heartbeat/framing and enables a fresh probe on demand', () => {
  const s = setup(false); assert.equal(s.route.status, 'disabled'); s.hello(); assert.equal(s.sockets.length, 0);
  assert.equal(JSON.parse(s.writes[0].data).v, 2);
  const decoded = [];
  const parser = new Deframer((type, p) => decoded.push([type, p.toString()]), () => {});
  for (const byte of frame(F.CONFIG, '{"cmd":"info"}')) parser.push(Buffer.from([byte]));
  assert.deepEqual(decoded, [[F.CONFIG, '{"cmd":"info"}']]);
  s.route.setEnabled(true); s.hello(); assert.equal(s.sockets.length, 1);
  s.sockets[0].open(); s.sockets[0].welcome();
  s.route.setEnabled(false); assert(s.sockets[0].dead); assert.equal(s.route.status, 'disabled');
  s.clock.tick(60000); assert.equal(s.sockets.length, 1);
});
test('hung ready upstream expires without heartbeat incorrectly claiming availability', () => {
  const s = setup(); s.hello(); s.sockets[0].open(); s.sockets[0].welcome();
  s.clock.tick(5000); assert.equal(s.sockets[0].pings, 1);
  s.clock.tick(3000); assert.equal(s.route.status, 'unavailable'); assert(s.sockets[0].dead);
  s.route.detach();
});
test('healthy pongs retain one authenticated socket and data uses only current epoch', () => {
  const s = setup(); s.hello(); s.sockets[0].open(); s.sockets[0].welcome();
  for (let i = 0; i < 5; i++) { s.clock.tick(5000); s.sockets[0].emit('pong'); }
  assert.equal(s.sockets.length, 1); assert.equal(s.route.status, 'ready');
  const bytes = Buffer.from([1, 2, 3, 4]);
  assert.deepEqual(s.route.deviceFrame(F.AUDIO, routedPayload(s.route.epoch, bytes)), bytes);
  assert.equal(s.route.deviceFrame(F.AUDIO, routedPayload(s.route.epoch - 1, bytes)), null);
  s.route.detach();
});
test('real WebSocket sends exactly one auth hello; duplicate USB probe replays welcome without session replacement', async () => {
  const server = new WebSocketServer({ port: 0 });
  await new Promise((resolve) => server.once('listening', resolve));
  let connections = 0, hellos = 0;
  server.on('connection', (socket) => {
    connections++;
    socket.on('message', (data) => {
      if (JSON.parse(data).t === 'hello') { hellos++; socket.send('{"t":"welcome","session":"real"}'); }
    });
  });
  let resolveReady;
  const ready = new Promise((resolve) => { resolveReady = resolve; });
  const route = new UsbRoute({ server: `ws://127.0.0.1:${server.address().port}/kubik/v1`, epoch: 1,
    send: (type, data) => { if (type === F.JSON) resolveReady(data); } });
  try {
    route.attach(); route.deviceFrame(F.JSON, routedPayload(route.epoch, hello));
    const data = await ready; assert.equal(JSON.parse(data.subarray(4)).session, 'real');
    route.deviceFrame(F.JSON, routedPayload(route.epoch, hello));
    await new Promise((resolve) => setTimeout(resolve, 30));
    assert.equal(connections, 1); assert.equal(hellos, 1);
  } finally { route.detach(); await new Promise((resolve) => server.close(resolve)); }
});


test('device configured server is default; explicit override wins and bridge hint is stripped upstream', () => {
  for (const override of [undefined, 'ws://override.example/kubik/v1']) {
    const targets = [], sockets = [], clock = new Clock();
    const route = new UsbRoute({ server: override, epoch: 8, send: () => {}, setTimer: clock.set, clearTimer: clock.clear,
      socketFactory: (url) => { targets.push(url); const socket = new Socket(); sockets.push(socket); return socket; } });
    route.attach();
    const data = JSON.stringify({ ...JSON.parse(hello), server: 'wss://device.example/kubik/v1' });
    route.deviceFrame(F.JSON, routedPayload(route.epoch, data));
    assert.equal(targets[0], override ?? 'wss://device.example/kubik/v1');
    sockets[0].open(); assert.equal(JSON.parse(sockets[0].sent[0].data).server, undefined);
    route.detach();
  }
});
test('missing or malformed device URL stays unavailable with an actionable reason and no socket', () => {
  for (const server of ['garbage', 'kubik://', 'kubik://host/path', 'kubik://user@host', 'kubik://host:0', 'https://server.example/', 'wss://user:secret@server.example/', 'ws://server/#fragment', 'ws://server', 'ws://bad_host/kubik/v1', 'ws://server:0/kubik/v1']) {
    const logs = [], clock = new Clock(); let opened = false;
    const route = new UsbRoute({ epoch: 9, send: () => {}, log: (s) => logs.push(s), setTimer: clock.set, clearTimer: clock.clear,
      socketFactory: () => { opened = true; return new Socket(); } });
    route.attach(); route.deviceFrame(F.JSON, routedPayload(route.epoch, JSON.stringify({ ...JSON.parse(hello), server })));
    assert.equal(opened, false); assert.equal(route.status, 'unavailable');
    assert.match(logs.join(' '), /invalid server/); assert.doesNotMatch(logs.join(' '), /secret/);
    route.detach();
  }
  assert.equal(serverUrl('ws://localhost:18790/kubik/v1'), 'ws://localhost:18790/kubik/v1');
});
test('handshake: challenge and pair are relayed with the epoch, auth goes upstream, pairing lifts the auth bound', () => {
  const s = setup();
  const v2 = JSON.stringify({ t: 'hello', v: 2, device: 'kubik-test', key: 'BAAA', fw: 'test' });
  const sendHello = () => s.route.deviceFrame(F.JSON, routedPayload(s.route.epoch, v2));
  const lastJson = () => JSON.parse(s.writes.filter((w) => w.type === F.JSON).at(-1).data.subarray(4));
  sendHello(); s.sockets[0].open();
  assert.equal(JSON.parse(s.sockets[0].sent[0].data).v, 2);
  s.sockets[0].emit('message', Buffer.from('{"t":"challenge","nonce":"bm9uY2U="}'), false);
  assert.deepEqual(lastJson(), { t: 'challenge', nonce: 'bm9uY2U=' });
  assert.equal(s.writes.at(-1).data.readUInt32LE(0), s.route.epoch);
  const writes = s.writes.length;
  sendHello(); // repeated identical hello before auth: the unanswered challenge is replayed, no new socket
  assert.equal(s.sockets.length, 1); assert.equal(s.writes.length, writes + 1); assert.equal(lastJson().t, 'challenge');
  const auth = Buffer.from('{"t":"auth","sig":"MEUCIQ=="}');
  assert.deepEqual(s.route.deviceFrame(F.JSON, routedPayload(s.route.epoch, auth)), auth);
  assert.equal(s.sockets[0].sent.at(-1).data, auth.toString());
  assert.equal(s.sockets[0].sent.at(-1).opts?.binary, false, 'auth is a text frame');
  assert.equal(s.route.deviceFrame(F.JSON, routedPayload(s.route.epoch, auth)), null, 'one auth per challenge');
  s.sockets[0].emit('message', Buffer.from('{"t":"pair","code":"ABCD2345"}'), false);
  assert.deepEqual(lastJson(), { t: 'pair', code: 'ABCD2345' });
  s.clock.tick(60000); assert.equal(s.route.status, 'probing', 'pending approval is not an auth timeout');
  s.sockets[0].welcome(); assert.equal(s.route.status, 'ready'); assert.equal(lastJson().t, 'welcome');
  sendHello(); assert.equal(s.sockets.length, 1); assert.equal(lastJson().t, 'welcome');
  s.route.detach();
});
test('handshake: a new server connection needs a fresh challenge, not the cached welcome', () => {
  const s = setup();
  const v2 = JSON.stringify({ t: 'hello', v: 2, device: 'kubik-test', key: 'BAAA', fw: 'test' });
  s.route.deviceFrame(F.JSON, routedPayload(s.route.epoch, v2)); s.sockets[0].open(); s.sockets[0].welcome();
  s.sockets[0].emit('close', 1006);
  assert.equal(s.route.status, 'unavailable'); assert.equal(s.route.welcome, null);
  s.clock.tick(2000);
  s.route.deviceFrame(F.JSON, routedPayload(s.route.epoch, v2)); assert.equal(s.sockets.length, 2);
  s.sockets[1].open(); assert.equal(JSON.parse(s.sockets[1].sent[0].data).t, 'hello');
  s.sockets[1].emit('message', Buffer.from('{"t":"challenge","nonce":"bmV3"}'), false);
  assert.equal(JSON.parse(s.writes.at(-1).data.subarray(4)).nonce, 'bmV3');
  s.route.detach();
});
test('a frame cut short on the device does not swallow the logs after it', () => {
  let t = 0; const got = [];
  const parser = new Deframer((type, p) => got.push(p.toString()), () => {}, { now: () => t });
  const cut = frame(F.LOG, 'x'.repeat(200)).subarray(0, 40);
  parser.push(cut); parser.push(frame(F.LOG, 'one'));
  t = 600; parser.push(frame(F.LOG, 'two'));
  assert.deepEqual(got, ['one', 'two']);
  parser.push(Buffer.from([0xa5, 0x5a, F.LOG, 0xff, 0xff])); parser.push(frame(F.LOG, 'three'));
  assert.deepEqual(got, ['one', 'two', 'three']);
});

test('server setting decides the upstream and the binding the device will sign', () => {
  assert.deepEqual(serverTarget(undefined), { discover: true, bind: 'spki' });
  assert.deepEqual(serverTarget(''), { discover: true, bind: 'spki' });
  assert.deepEqual(serverTarget('kubik://192.168.1.20'), { url: 'wss://192.168.1.20:18790/kubik/v1', bind: 'spki' });
  assert.deepEqual(serverTarget('kubik://kubik-host.lan:4443/'), { url: 'wss://kubik-host.lan:4443/kubik/v1', bind: 'spki' });
  assert.deepEqual(serverTarget('kubik://[fd00::5]'), { url: 'wss://[fd00::5]:18790/kubik/v1', bind: 'spki' });
  assert.deepEqual(serverTarget('wss://claw.example/kubik/v1'), { url: 'wss://claw.example/kubik/v1', bind: 'ca:claw.example' });
  assert.deepEqual(serverTarget('wss://Claw.Example:8443/kubik/v1'), { url: 'wss://Claw.Example:8443/kubik/v1', bind: 'ca:claw.example' });
  assert.deepEqual(serverTarget('wss://[FD00::5]:8443/kubik/v1'), { url: 'wss://[FD00::5]:8443/kubik/v1', bind: 'ca:fd00::5' });
  assert.deepEqual(serverTarget('wss://192.0.2.7/kubik/v1').bind, 'ca:192.0.2.7');
  assert.deepEqual(serverTarget('ws://127.0.0.1:18999/kubik/v1'), { url: 'ws://127.0.0.1:18999/kubik/v1', bind: 'none' });
});
test('binding frame 0x23 reaches the device with the epoch before the hello goes upstream', () => {
  for (const [server, bind] of [['ws://test/kubik/v1', 'none'], ['wss://test/kubik/v1', 'ca:test'], ['wss://Test.example:8443/kubik/v1', 'ca:test.example']]) {
    const writes = [], sockets = [], clock = new Clock(), order = [];
    const route = new UsbRoute({ server, epoch: 40, setTimer: clock.set, clearTimer: clock.clear,
      send: (type, data) => { writes.push({ type, data }); if (type === F.BIND) order.push('bind'); },
      socketFactory: () => { const socket = new Socket(); socket.send = (data) => { order.push(JSON.parse(data).t); }; sockets.push(socket); return socket; } });
    route.attach(); route.deviceFrame(F.JSON, routedPayload(route.epoch, hello)); sockets[0].open();
    const frameOut = writes.find((w) => w.type === F.BIND);
    assert.equal(frameOut.data.readUInt32LE(0), route.epoch);
    assert.equal(frameOut.data.subarray(4).toString('ascii'), bind);
    assert.deepEqual(order, ['bind', 'hello']);
    route.detach();
  }
});
test('LAN target: TLS without CA check, the binding is the SPKI hash of the certificate the bridge saw', () => {
  const writes = [], configs = [], sockets = [], clock = new Clock();
  const route = new UsbRoute({ server: 'kubik://10.0.0.7', epoch: 50, setTimer: clock.set, clearTimer: clock.clear,
    send: (type, data) => writes.push({ type, data }),
    socketFactory: (url, cfg) => { configs.push([url, cfg.rejectUnauthorized]); const s = new Socket(); sockets.push(s); return s; } });
  route.attach(); route.deviceFrame(F.JSON, routedPayload(route.epoch, hello));
  assert.deepEqual(configs, [['wss://10.0.0.7:18790/kubik/v1', false]]);
  sockets[0].open(); // no certificate observed: never guess a binding
  assert.equal(route.status, 'unavailable'); assert.equal(writes.some((w) => w.type === F.BIND), false);
  route.detach();
});
test('empty server: LAN discovery picks the upstream; no answer leaves the route unavailable', async () => {
  const targets = [], logs = [];
  const route = new UsbRoute({ epoch: 60, send: () => {}, log: (m) => logs.push(m), discover: async () => 'wss://192.168.1.9:18790/kubik/v1',
    socketFactory: (url) => { targets.push(url); return new Socket(); } });
  route.attach(); route.deviceFrame(F.JSON, routedPayload(route.epoch, hello));
  route.deviceFrame(F.JSON, routedPayload(route.epoch, hello)); // a repeated hello while discovering starts nothing new
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(targets, ['wss://192.168.1.9:18790/kubik/v1']);
  route.detach();
  const failing = new UsbRoute({ epoch: 61, send: () => {}, log: (m) => logs.push(m), discover: async () => { throw new Error('no Kubik server answered LAN discovery'); },
    socketFactory: () => assert.fail('no socket without a server') });
  failing.attach(); failing.deviceFrame(F.JSON, routedPayload(failing.epoch, hello));
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(failing.status, 'unavailable'); assert.match(logs.join(' '), /LAN discovery/);
  failing.detach();
});
test('real UDP discovery: request text, JSON reply, URL from the reply source address', async () => {
  const responder = createSocket('udp4'); const seen = [];
  responder.on('message', (message, peer) => {
    seen.push(message.toString());
    responder.send('not json', peer.port, peer.address);
    responder.send(JSON.stringify({ t: 'kubik', v: 5, port: 4443 }), peer.port, peer.address);
  });
  await new Promise((resolve) => responder.bind(0, '127.0.0.1', resolve));
  try {
    assert.equal(await discoverServer({ address: '127.0.0.1', port: responder.address().port }), 'wss://127.0.0.1:4443/kubik/v1');
    assert.deepEqual(seen, [DISCOVERY_REQUEST]);
  } finally { responder.close(); }
});
