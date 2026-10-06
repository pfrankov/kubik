// USB tether end to end: the real bridge route (tools/usb-bridge) is the TLS client of the real LAN listener and
// reports the binding to a simulated device (frame 0x23), which checks its pin and signs it.
import assert from 'node:assert/strict';
import { connect as connectTls, createServer as createTlsServer } from 'node:tls';
import test from 'node:test';
import { F, routedPayload } from '../../tools/usb-bridge/bridge-protocol.mjs';
import { discoverServer } from '../../tools/usb-bridge/lan-discovery.mjs';
import { UsbRoute } from '../../tools/usb-bridge/usb-route.mjs';
import { loadTlsIdentity } from '../src/tls-identity.js';
import { DEFAULT_DEVICE_KEY } from './helpers.js';
import { start, stateDir } from './server-fixture.js';

/** A device on the USB side: TOFU or a stored pin, like the firmware rules in docs/protocol.md. */
function usbDevice({ pin = null } = {}) {
  const device = { binds: [], events: [], refused: false };
  let route;
  device.attach = (options) => {
    route = new UsbRoute({ epoch: 7, log: () => {}, ...options, send: (type, data) => device.receive(type, data) });
    device.route = route;
    route.attach();
    route.deviceFrame(F.JSON, routedPayload(route.epoch, JSON.stringify(DEFAULT_DEVICE_KEY.hello())));
  };
  device.receive = (type, data) => {
    if (type === F.HOST_HELLO || data.readUInt32LE(0) !== route.epoch) return;
    const payload = data.subarray(4);
    if (type === F.BIND) { device.binds.push(payload.toString('ascii')); return; }
    if (type !== F.JSON) return;
    const message = JSON.parse(payload);
    device.events.push(message);
    if (message.t !== 'challenge') return;
    const bind = device.binds.at(-1);
    if (!bind || (pin && bind !== pin)) { device.refused = true; return; } // "server key changed": no auth
    route.deviceFrame(F.JSON, routedPayload(route.epoch, JSON.stringify({ t: 'auth', sig: DEFAULT_DEVICE_KEY.sign(message.nonce, { bind }) })));
  };
  device.until = async (match, timeoutMs = 4000) => {
    for (const end = Date.now() + timeoutMs; Date.now() < end; await new Promise((r) => setTimeout(r, 10))) if (match()) return;
    throw new Error(`timeout; events ${JSON.stringify(device.events)} binds ${device.binds}`);
  };
  return device;
}

test('USB bridge as TLS client: binding frame precedes the challenge, the device signs the LAN SPKI, welcome', async (t) => {
  const { lan } = await start(t);
  const device = usbDevice({ pin: lan.spki });
  device.attach({ server: `kubik://127.0.0.1:${lan.port}` });
  try {
    await device.until(() => device.route.status === 'ready');
    assert.deepEqual(device.binds, [lan.spki]);
    assert.deepEqual(device.events.slice(0, 2).map((e) => e.t), ['challenge', 'welcome']);
  } finally { device.route.detach(); }
});

test('USB bridge with an empty device server finds the plugin by LAN discovery', async (t) => {
  const { lan } = await start(t);
  const device = usbDevice();
  device.attach({ discover: () => discoverServer({ address: '127.0.0.1', port: lan.discovery }) });
  try {
    await device.until(() => device.route.status === 'ready');
    assert.deepEqual(device.binds, [lan.spki]);
  } finally { device.route.detach(); }
});

test('USB bridge through a relay: a pinned device refuses; a TOFU device signs the relay key and gets 4001', async (t) => {
  const { lan } = await start(t);
  const identity = loadTlsIdentity(stateDir(t));
  const relay = createTlsServer({ key: identity.key, cert: identity.cert }, (client) => {
    const upstream = connectTls({ host: '127.0.0.1', port: lan.port, rejectUnauthorized: false });
    const kill = () => { client.destroy(); upstream.destroy(); };
    for (const socket of [client, upstream]) { socket.on('error', kill); socket.on('close', kill); }
    client.pipe(upstream).pipe(client);
  });
  await new Promise((resolve) => relay.listen(0, '127.0.0.1', resolve));
  t.after(() => relay.close());
  const server = `kubik://127.0.0.1:${relay.address().port}`;

  const pinned = usbDevice({ pin: lan.spki });
  pinned.attach({ server });
  try {
    await pinned.until(() => pinned.refused);
    assert.deepEqual(pinned.binds, [identity.spkiHash]);
  } finally { pinned.route.detach(); }

  const trusting = usbDevice(); // the server's error frame is not relayed while probing; the close is what counts
  const closes = [];
  trusting.attach({ server, log: (line) => closes.push(line) });
  try {
    await trusting.until(() => trusting.route.status === 'unavailable');
    assert.deepEqual(trusting.binds, [identity.spkiHash]);
    assert.deepEqual(trusting.events.map((e) => e.t), ['challenge']);
    assert.match(closes.join(' '), /closed 4001/);
  } finally { trusting.route.detach(); }
});
