import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createRequire } from 'node:module';
import { isIP } from 'node:net';
import { execFile, spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
const require = createRequire(new URL('../../openclaw-kubik/package.json', import.meta.url));
const { WebSocketServer } = require('ws');

const path = '/kubik/v1';
let originClientIp = '';
const server = new WebSocketServer({ host: '127.0.0.1', port: 0, path });
await new Promise((resolve, reject) => {
  server.once('listening', resolve);
  server.once('error', reject);
});
server.on('connection', (socket, request) => {
  if (request.url !== path) return socket.close();
  originClientIp = request.headers['x-forwarded-for'];
  socket.once('message', (data) => {
    let hello;
    try { hello = JSON.parse(data.toString()); } catch { return socket.close(); }
    if (hello.t !== 'hello' || hello.v !== 4 || hello.fw !== '0.6.1' || !hello.key) return socket.close();
    socket.send(JSON.stringify({ t: 'challenge', nonce: randomBytes(32).toString('base64') }));
  });
});

const tunnel = spawn(process.env.CLOUDFLARED_BIN || 'cloudflared', ['tunnel', '--no-autoupdate', '--url', `http://127.0.0.1:${server.address().port}`], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
let logs = '';
const findHostname = new Promise((resolve, reject) => {
  const timeout = setTimeout(() => reject(new Error(`Quick Tunnel did not report a URL: ${logs.slice(-500)}`)), 45_000);
  const onOutput = (chunk) => {
    logs = `${logs}${chunk}`.slice(-4000);
    const hostname = logs.match(/https:\/\/([a-z0-9-]+\.trycloudflare\.com)/i)?.[1];
    if (hostname) { clearTimeout(timeout); resolve(hostname); }
  };
  tunnel.stdout.on('data', onOutput);
  tunnel.stderr.on('data', onOutput);
  tunnel.once('error', (error) => { clearTimeout(timeout); reject(error); });
  tunnel.once('exit', (code) => { clearTimeout(timeout); reject(new Error(`cloudflared exited ${code}: ${logs.slice(-500)}`)); });
});

try {
  const hostname = await findHostname;
  let reached = false;
  let lastError;
  const deadline = Date.now() + 60_000;
  while (!reached && Date.now() < deadline) {
    await delay(1500);
    const remaining = deadline - Date.now();
    if (remaining <= 0) break;
    try {
      await new Promise((resolve, reject) => execFile(process.execPath,
        [fileURLToPath(new URL('./probe.mjs', import.meta.url)), `wss://${hostname}${path}`],
        { timeout: Math.min(12_000, remaining) }, (error) => error ? reject(error) : resolve()));
      reached = true;
    } catch (error) { lastError = error; }
  }
  if (!reached) throw lastError ?? new Error('WSS mock did not return a challenge');
  assert.ok(isIP(originClientIp), `Cloudflare origin client IP header missing or invalid: ${originClientIp}`);
  console.log('Cloudflare Quick Tunnel Kubik challenge smoke passed for /kubik/v1 with a local mock and client-IP header.');
} finally {
  tunnel.kill('SIGTERM');
  await new Promise((resolve) => {
    const timeout = setTimeout(resolve, 5000);
    tunnel.once('exit', () => { clearTimeout(timeout); resolve(); });
  });
  await new Promise((resolve) => server.close(resolve));
}
