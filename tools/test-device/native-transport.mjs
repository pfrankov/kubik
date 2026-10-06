import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { isIP } from 'node:net';
import { createServer } from 'node:http';
import { LanListener } from '../../openclaw-kubik/src/lan.js';
import { loadTlsIdentity } from '../../openclaw-kubik/src/tls-identity.js';
import { startRadioRelay } from '../radio-relay.mjs';

/** USB control remains separate from the authenticated audio route under test. */
export async function nativeTransport(server, original, wifi) {
  if (!wifi) {
    const http = createServer();
    http.on('upgrade', (req, socket, head) => server.handleUpgrade(req, socket, head,
      { via: 'usb', bind: 'none', remote: '127.0.0.1', hint: '' }));
    await new Promise((resolve, reject) => { http.once('error', reject); http.listen(18999, '127.0.0.1', resolve); });
    return { via: 'usb', enter: sim => sim('wifi', { ms: 120000 }),
      restore: sim => sim('wifi', { ms: 1 }), close: async () => { http.close(); } };
  }
  const host = process.env.KUBIK_TEST_LAN_HOST;
  assert.equal(isIP(host), 4, 'Set KUBIK_TEST_LAN_HOST to a reachable local IPv4');
  assert.equal(original.server_pinned, false, 'Cannot restore an existing LAN pin after endpoint replacement');
  assert.match(original.url, /^wss:\/\//);
  const port = Number(process.env.KUBIK_TEST_LAN_PORT ?? 0);
  assert.ok(Number.isInteger(port) && port >= 0 && port <= 65535, 'Invalid fixture listener port');
  const peers = process.env.KUBIK_TEST_LAN_PEERS?.split(',');
  assert.ok(!peers || (peers.length <= 4 && peers.every(peer => isIP(peer) === 4)), 'Invalid explicit fixture peers');
  const dir = mkdtempSync(join(tmpdir(), 'kubik-native-tls-'));
  const listener = new LanListener({ identity: loadTlsIdentity(dir), port, host, discoveryPort: 0, discoveryHost: host,
    ...(peers ? { isAllowedPeer: address => peers.includes(address) } : {}),
    onUpgrade: (req, socket, head, transport) => server.handleUpgrade(req, socket, head, transport) });
  let relay;
  const close = async () => {
    try { await relay?.close(); } finally { try { await listener.stop(); } finally { rmSync(dir, { recursive: true, force: true }); } }
  };
  try { await listener.start(); relay = await startRadioRelay(host, listener.port); }
  catch (error) { await close(); throw error; }
  return { via: 'wifi', enter: (_sim, control) => control({ cmd: 'set', url: relay?.url ?? `kubik://${host}:${listener.port}` }),
    restore: (_sim, control) => control({ cmd: 'set', url: original.url }), close };
}
