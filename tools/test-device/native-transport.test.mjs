import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:net';
import test from 'node:test';
import { nativeTransport } from './native-transport.mjs';
import { loadTlsIdentity } from '../../openclaw-kubik/src/tls-identity.js';
import { KubikServer } from '../../openclaw-kubik/src/server.js';
import { account, connectDevice, DEFAULT_DEVICE_KEY, deviceKey, fakeEngine } from '../../openclaw-kubik/test/helpers.js';

async function vacantPort() {
  const server = createServer();
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  await new Promise(resolve => server.close(resolve));
  return port;
}

test('pinned Wi-Fi retains exact TLS identity and signed device allowlist without routing writes', { timeout: 8000 }, async () => {
  const directory = mkdtempSync(join(tmpdir(), 'kubik-pinned-transport-'));
  const identity = loadTlsIdentity(directory), before = readFileSync(identity.path);
  const port = await vacantPort(), pinnedEndpoint = `kubik://saved.example:${port}`;
  const original = { url: pinnedEndpoint, online: false, via: 'none', server_pinned: true };
  const seen = [];
  const server = new KubikServer({ account: account(), engineFactory: () => fakeEngine(),
    pairing: { allowed: async () => [DEFAULT_DEVICE_KEY.entry], upsert: () => { throw Error('Unapproved fixture device'); } },
    dispatch: () => assert.fail('No voice turn was requested') });
  const upgrade = server.handleUpgrade.bind(server);
  server.handleUpgrade = (...args) => { seen.push(args[3]); return upgrade(...args); };
  let transport, accepted, denied;
  server.start();
  try {
    transport = await nativeTransport(server, original, true, { identityPath: identity.path, pinnedEndpoint });
    const forbidWrite = () => assert.fail('Pinned fixture must not change radio routing or saved settings');
    await transport.enter(forbidWrite, forbidWrite);
    assert.equal(transport.via, 'wifi');
    accepted = await connectDevice(`wss://127.0.0.1:${port}/kubik/v1`, { bind: 'seen' });
    assert.ok(accepted.events.some(event => event.t === 'welcome'));
    assert.equal(seen[0].via, 'lan');
    assert.equal(seen[0].bind, identity.spkiHash);
    const foreign = deviceKey('kubik-foreign');
    denied = await connectDevice(`wss://127.0.0.1:${port}/kubik/v1`, {
      hello: foreign.hello(), auth: nonce => foreign.sign(nonce, { bind: identity.spkiHash }) });
    assert.ok(!denied.events.some(event => event.t === 'welcome'));
    assert.equal(server.getSession(foreign.device), undefined);
    await transport.restore(forbidWrite, forbidWrite);
    assert.deepEqual(readFileSync(identity.path), before);
  } finally {
    accepted?.close(); denied?.close(); server.stop();
    await transport?.close(); rmSync(directory, { recursive: true, force: true });
  }
});

test('unpinned local Wi-Fi retains its deliberate endpoint replacement contract', { timeout: 5000 }, async () => {
  const names = ['KUBIK_TEST_LAN_HOST', 'KUBIK_TEST_LAN_PORT', 'KUBIK_TEST_LAN_PEERS',
    'KUBIK_TEST_RELAY_SSH', 'KUBIK_TEST_RELAY_IPV4'];
  const before = Object.fromEntries(names.map(name => [name, process.env[name]]));
  for (const name of names) delete process.env[name];
  process.env.KUBIK_TEST_LAN_HOST = '127.0.0.1';
  const writes = [], original = { server_pinned: false, url: 'wss://original.example/kubik/v1' };
  let transport;
  try {
    transport = await nativeTransport({ handleUpgrade: () => assert.fail('Unexpected client') }, original, true,
      { identityPath: undefined, pinnedEndpoint: undefined });
    const control = async value => writes.push(value);
    const sim = () => assert.fail('LAN replacement must not pause Wi-Fi');
    await transport.enter(sim, control); await transport.restore(sim, control);
    assert.equal(writes[0].cmd, 'set'); assert.match(writes[0].url, /^kubik:\/\/127\.0\.0\.1:\d+$/);
    assert.deepEqual(writes[1], { cmd: 'set', url: original.url });
  } finally {
    await transport?.close();
    for (const name of names) { if (before[name] === undefined) delete process.env[name]; else process.env[name] = before[name]; }
  }
});
