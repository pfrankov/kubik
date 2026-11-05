import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { connect } from 'node:net';
import { test } from 'node:test';
import { WebSocketServer, WebSocket } from 'ws';
import { loadTlsIdentity } from '../../openclaw-kubik/src/tls-identity.js';
import { spkiSha256 } from '../../openclaw-kubik/src/protocol.js';
import { createUsbListener } from './usb-listener.mjs';

function directory(t) {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-usb-listener-test-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
}
async function open(listener, t) {
  const wss = new WebSocketServer({ server: listener.http });
  wss.on('connection', ws => ws.send('fixture ready'));
  const port = await listener.listen(0);
  t.after(async () => { await listener.close(); await new Promise(resolve => wss.close(resolve)); });
  const ws = new WebSocket(`${listener.protocol}://127.0.0.1:${port}`, { rejectUnauthorized: false });
  t.after(() => ws.terminate());
  await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); });
  return { ws, port };
}

test('plain mode requires an explicit permitted CA-device fixture and binds none', { timeout: 5000 }, async t => {
  assert.throws(() => createUsbListener({ identityPath: undefined }), /LAN devices require/);
  const listener = createUsbListener({ identityPath: undefined, allowPlain: true });
  assert.equal(listener.protocol, 'ws'); assert.equal(listener.bind, 'none');
  const { ws } = await open(listener, t);
  assert.equal(ws._socket.encrypted, undefined);
});

test('TLS handshake presents the exact selected identity and SPKI binding', { timeout: 5000 }, async t => {
  const identity = loadTlsIdentity(directory(t));
  const original = readFileSync(identity.path);
  const listener = createUsbListener({ identityPath: identity.path });
  assert.equal(listener.protocol, 'wss'); assert.equal(listener.bind, identity.spkiHash);
  const { ws, port } = await open(listener, t);
  assert.equal(spkiSha256(ws._socket.getPeerCertificate(true).raw), listener.bind);
  assert.deepEqual(readFileSync(identity.path), original, 'Selected identity must remain unchanged');
  const stalled = connect(port, '127.0.0.1');
  stalled.on('error', error => assert.equal(error.code, 'ECONNRESET'));
  await new Promise(resolve => stalled.once('connect', resolve));
  await listener.close();
  assert.equal(listener.http.listening, false);
});

test('missing, invalid, oversized and mismatched explicit identities never select plain mode', () => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-usb-identity-invalid-'));
  try {
    const identity = loadTlsIdentity(join(dir, 'one')), other = loadTlsIdentity(join(dir, 'two'));
    const path = join(dir, 'invalid.json');
    for (const data of ['{bad', '{}', JSON.stringify({ key: identity.key, cert: other.cert }), ' '.repeat(16385)]) {
      writeFileSync(path, data);
      assert.throws(() => createUsbListener({ identityPath: path, allowPlain: true }));
      assert.equal(readFileSync(path, 'utf8'), data, 'Invalid selected identity must not be repaired');
    }
    assert.throws(() => createUsbListener({ identityPath: join(dir, 'missing'), allowPlain: true }), /ENOENT/);
    assert.throws(() => createUsbListener({ identityPath: '', allowPlain: true }), /must name/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
