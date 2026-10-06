#!/usr/bin/env node
// Check a customer's named WSS route without approving a device or sending audio.
import { generateKeyPairSync, randomBytes } from 'node:crypto';
import { lookup } from 'node:dns/promises';

try {
  let address;
  try { address = new URL(process.argv[2]); } catch { throw new Error('Usage: node probe.mjs wss://your-domain/kubik/v1'); }
  if (address.protocol !== 'wss:' || address.pathname !== '/kubik/v1' || address.username || address.password ||
      address.search || address.hash) throw new Error('Use a WSS address ending in /kubik/v1, without credentials, query or fragment');
  if (typeof WebSocket !== 'function') throw new Error('Node.js 24 or newer is required for the WSS probe');
  try { await lookup(address.hostname); }
  catch (error) { throw new Error(`DNS cannot resolve ${address.hostname}: ${error.code ?? error.message}`); }

  const { publicKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const jwk = publicKey.export({ format: 'jwk' });
  const key = Buffer.concat([Buffer.from([4]), Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]).toString('base64');
  const device = `kubik-probe-${randomBytes(3).toString('hex')}`;
  await new Promise((resolve, reject) => {
    const socket = new WebSocket(address.href);
    let settled = false;
    const finish = (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      if (socket.readyState === WebSocket.OPEN) socket.close();
      if (error) reject(error); else resolve();
    };
    const timeout = setTimeout(() => finish(new Error('WSS challenge timed out')), 10_000);
    socket.onopen = () => socket.send(JSON.stringify({ t: 'hello', v: 4, fw: '0.6.1', device, key, name: 'WSS probe' }));
    socket.onerror = (event) => finish(new Error(`WSS connection failed: ${event.error?.message || 'check certificate, Tunnel and route'}`));
    socket.onclose = (event) => finish(new Error(`WSS closed before challenge (${event.code})`));
    socket.onmessage = (message) => {
      let event;
      try { event = JSON.parse(message.data); } catch { return finish(new Error('WSS endpoint returned invalid JSON')); }
      if (event.t !== 'challenge' || typeof event.nonce !== 'string' ||
          !/^[A-Za-z0-9+/]{43}=$/.test(event.nonce) || Buffer.from(event.nonce, 'base64').length !== 32) {
        return finish(new Error('WSS endpoint did not return a Kubik v4 challenge'));
      }
      finish();
    };
  });
  console.log('WSS route reached the Kubik v4 challenge. Pairing, audio and ESP32 TLS trust still need device checks.');
} catch (error) {
  console.error(`Kubik route check failed: ${error.message}`);
  process.exitCode = 1;
}
