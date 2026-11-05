#!/usr/bin/env node
// Run on the identity's owning host. A temporary USB/SSH route verifies real
// firmware with the existing pin; TLS private keys never leave this host.
import assert from 'node:assert/strict';
import { createHash, createPrivateKey, X509Certificate } from 'node:crypto';
import { readFile, stat } from 'node:fs/promises';
import { createServer } from 'node:https';
import { isAbsolute, join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as pause } from 'node:timers/promises';

const options = Object.fromEntries(process.argv.slice(2).map(arg => {
  const at = arg.indexOf('=');
  assert.ok(arg.startsWith('--') && at > 2, 'Use explicit --name=value arguments');
  return [arg.slice(2, at), arg.slice(at + 1)];
}));
for (const key of ['plugin', 'identity']) assert.ok(isAbsolute(options[key] ?? ''), `Explicit absolute ${key} is required`);
assert.match(options.device ?? '', /^[a-z0-9][a-z0-9_-]{0,63}$/);
assert.match(options.fingerprint ?? '', /^[0-9a-f]{32}$/);
assert.match(options.spki ?? '', /^[0-9a-f]{64}$/);
const port = Number(options.port);
assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
const load = name => import(pathToFileURL(join(options.plugin, 'src', name)).href);
const [{ KubikServer }, { resolveAccount }, { createPrinterToolFactory }] = await Promise.all([
  load('server.js'), load('config.js'), load('printer-tool.js'),
]);

async function identity() {
  try { return await readIdentity(); }
  catch { throw Error('Invalid TLS identity or unexpected pin; private contents are not logged'); }
}

async function readIdentity() {
  assert.ok((await stat(options.identity)).size <= 16384, 'Identity is too large');
  const stored = JSON.parse(await readFile(options.identity, 'utf8'));
  const certificate = new X509Certificate(stored.cert);
  assert.ok(certificate.checkPrivateKey(createPrivateKey(stored.key)), 'TLS key mismatch');
  const spki = createHash('sha256').update(certificate.publicKey.export({ type: 'spki', format: 'der' })).digest('hex');
  assert.equal(spki, options.spki, 'Refusing to change the device pin');
  return { key: stored.key, cert: stored.cert, minVersion: 'TLSv1.2' };
}

const cfg = { channels: { kubik: { enabled: true } } };
const account = resolveAccount(cfg, 'default', { env: {}, readSecrets: false });
const expected = `${options.device}:${options.fingerprint}`;
const server = new KubikServer({ account,
  pairing: { allowed: async () => [expected], upsert: async () => { throw Error('Unknown device'); } },
  engineFactory: () => ({ voiceMode: 'classic', canListen: false, canSpeak: false, close() {} }),
  dispatch: async () => { throw Error('This physical fixture must never call an agent'); },
});
const sockets = new Set();
const http = createServer(await identity(), (_request, response) => { response.writeHead(404); response.end(); });
http.on('connection', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
http.on('upgrade', (request, socket, head) => {
  if (request.url !== '/kubik/v1') { socket.destroy(); return; }
  server.handleUpgrade(request, socket, head, { via: 'lan', bind: options.spki, remote: '127.0.0.1' });
});

async function waitForSession() {
  for (let n = 0; n < 240; n++) {
    const session = server.getSession(options.device);
    if (session) {
      assert.equal(session.device.fingerprint, options.fingerprint);
      assert.equal(session.device.printer, 1, 'Firmware does not advertise the printer protocol');
      return session;
    }
    await pause(100);
  }
  throw Error('No authenticated physical device arrived within 24 seconds');
}

async function exercise() {
  const session = await waitForSession();
  let sends = 0;
  const send = session.send.bind(session);
  session.send = message => { if (message.t === 'printer') sends++; return send(message); };
  const route = { agentId: 'printer-fixture', sessionKey: `printer-fixture:${options.device}` };
  const core = { channel: { routing: { resolveAgentRoute: () => route } } };
  const tool = createPrinterToolFactory({ currentConfig: () => cfg, runtime: () => ({ core }), serverFor: () => server })({
    messageChannel: 'kubik', nativeChannelId: options.device, agentAccountId: 'default', ...route,
  });
  assert.ok(tool, 'The physical fixture did not obtain a scoped tool');
  const status = await tool.execute('physical-printer-status', { action: 'status' });
  assert.equal(status.details.status, 'not_configured', 'Fixture only exercises firmware without a printer driver');
  assert.equal(sends, 1);
  const params = { action: 'print', text: 'Printer protocol check\nNo driver configured.' };
  const first = await tool.execute('physical-unconfigured-print', params);
  assert.equal(first.details.status, 'not_configured');
  assert.equal(first.details.outcome, 'not_sent');
  assert.equal(first.details.paperConfirmed, false);
  assert.equal(first.isError, true);
  const again = await tool.execute('physical-unconfigured-print', params);
  assert.deepEqual(again, first);
  assert.equal(sends, 2, 'The same print call was sent twice');
  const invalid = await tool.execute('physical-invalid-print', { action: 'print', text: '\u001b@' });
  assert.equal(invalid.details.code, 'invalid_input');
  assert.equal(sends, 2, 'Control bytes reached the device');
  console.log(JSON.stringify({ phase: 'complete', device: options.device, status: status.details.status,
    print: first.details.status, outcome: first.details.outcome, paperConfirmed: false,
    wireRequests: sends, duplicateSuppressed: true, controlRejected: true }));
}

const deadline = setTimeout(() => { console.error('Physical printer fixture exceeded 40 seconds'); process.exit(1); }, 40000);
try {
  server.start();
  await new Promise((resolve, reject) => { http.once('error', reject); http.listen(port, '127.0.0.1', resolve); });
  console.log(JSON.stringify({ phase: 'ready', port, spki: options.spki }));
  await exercise();
} finally {
  for (const socket of sockets) socket.destroy();
  await server.stop();
  await new Promise(resolve => http.close(resolve));
  clearTimeout(deadline);
}
