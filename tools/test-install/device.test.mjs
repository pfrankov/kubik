import assert from 'node:assert/strict';
import { EventEmitter, once } from 'node:events';
import { mkdtempSync, rmSync } from 'node:fs';
import { createServer } from 'node:https';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { WebSocketServer } from 'ws';
import { SimDevice, newDeviceKeyPem } from './device.mjs';
import { loadTlsIdentity } from '../../openclaw-kubik/src/tls-identity.js';
import { ImaEncoder, parseDeviceMessage, speechFrameIma } from '../../openclaw-kubik/src/protocol.js';

test('install device reports integer progress for whole and fractional PCM milliseconds', { timeout: 5000 }, async (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-device-progress-'));
  const server = createServer(loadTlsIdentity(dir));
  const sockets = new WebSocketServer({ server });
  const messages = new EventEmitter();
  const device = new SimDevice({ id: 'kubik-abcdef', keyPem: newDeviceKeyPem() });
  t.after(() => { device.close(); for (const socket of sockets.clients) socket.terminate();
    sockets.close(); server.close(); rmSync(dir, { recursive: true, force: true }); });
  sockets.on('connection', socket => socket.on('message', data => {
    const event = JSON.parse(data);
    if (event.t === 'progress' || event.t === 'played') messages.emit(event.t, event);
  }));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const connected = once(sockets, 'connection');
  await device.connect(`wss://127.0.0.1:${server.address().port}/kubik/v1`);
  const [peer] = await connected;
  // Delay speak_end until the progress timer reaches the short final fragment.
  // 48 B is one whole ms; 52 B includes a partial ms and used to close the session.
  for (const [gen, tailBytes] of [[1, 48], [255, 52]]) {
    const progress = once(messages, 'progress');
    peer.send(JSON.stringify({ t: 'speak', gen, kind: 'notify' }));
    const encoder = new ImaEncoder();
    for (const size of [480, tailBytes]) peer.send(speechFrameIma(gen, encoder.encode(Buffer.alloc(size))));
    const [report] = await progress;
    assert.deepEqual(parseDeviceMessage(JSON.stringify(report)), { t: 'progress', gen, ms: 11 });
    const played = once(messages, 'played');
    peer.send(JSON.stringify({ t: 'speak_end', gen }));
    const [receipt] = await played;
    assert.deepEqual(parseDeviceMessage(JSON.stringify(receipt)), { t: 'played', gen, ms: 11 });
  }
});
