// The gateway route binds the device signature to the host the request arrived on (`ca:<host>`).
import assert from 'node:assert/strict';
import { connect } from 'node:net';
import test from 'node:test';
import { isTrustedProxyAddress } from 'openclaw/plugin-sdk/core';
import { normalizeHostHeader, requestBindHost } from '../src/bind-host.js';
import { connectDevice } from './helpers.js';
import { start } from './server-fixture.js';

const outcome = (device) => device.waitFor((event) => ['welcome', 'pair', 'error'].includes(event.t), 5000, true);
const trusting = (...proxies) => ({ isTrusted: isTrustedProxyAddress, proxies: () => proxies });
const request = (headers) => ({ headers, rawHeaders: Object.entries(headers).flat() });

/** Connects a device that signed `bind` (and says `fw`) with extra request headers; resolves the server's verdict. */
async function verdict(url, headers, bind, fw) {
  const device = await connectDevice(url, { headers, bind, ...(fw === undefined ? {} : { fw }) });
  const event = await outcome(device);
  device.close();
  return event?.t === 'error' ? event.code : event?.t;
}

test('Host: lowercased, port and one trailing dot dropped, IPv6 without brackets; the device signs exactly that', async (t) => {
  const { url } = await start(t);
  assert.equal(await verdict(url, { Host: 'Kubik.Example.COM.:8443' }, 'ca:kubik.example.com'), 'welcome');
  assert.equal(await verdict(url, { Host: '[2001:DB8::1]:8443' }, 'ca:2001:db8::1'), 'welcome');
  assert.equal(await verdict(url, { Host: 'kubik.example.com' }, 'ca:kubik.example.com.'), 'unauthorized', 'the dot is not part of the bind');
  assert.equal(await verdict(url, { Host: 'kubik.example.com:8443' }, 'ca:kubik.example.com:8443'), 'unauthorized', 'nor is the port');
  assert.equal(await verdict(url, { Host: '[::1]:8443' }, 'ca:[::1]'), 'unauthorized', 'nor the IPv6 brackets');
});

test('a handshake relayed from another host is refused and the log says which host the request arrived on', async (t) => {
  const { url, logs } = await start(t);
  assert.equal(await verdict(url, { Host: 'kubik.example.com' }, 'ca:attacker.example'), 'unauthorized');
  const line = logs.find((entry) => /rejected device/.test(entry));
  assert.match(line, /transport binding mismatch/);
  assert.match(line, /this request arrived on host "kubik\.example\.com" \(Host header\)/);
});

test('X-Forwarded-Host counts only from a trusted proxy peer, and only when the proxy rewrote Host to its upstream', async (t) => {
  const { url } = await start(t, { trust: trusting('127.0.0.1') });
  const behind = { Host: '127.0.0.1:18789', 'X-Forwarded-Host': 'Kubik.Example.com:443' };
  assert.equal(await verdict(url, behind, 'ca:kubik.example.com'), 'welcome');
  assert.equal(await verdict(url, behind, 'ca:127.0.0.1'), 'unauthorized', 'the rewritten Host is not what the device connected to');
  assert.equal(await verdict(url, { Host: 'openclaw:18789', 'X-Forwarded-Host': 'kubik.example.com' }, 'ca:kubik.example.com'), 'welcome', 'single-label upstream name');
  // A comma list: the last value is the one the trusted proxy appended.
  const chain = { Host: '127.0.0.1', 'X-Forwarded-Host': 'attacker.example, kubik.example.com' };
  assert.equal(await verdict(url, chain, 'ca:kubik.example.com'), 'welcome');
  assert.equal(await verdict(url, chain, 'ca:attacker.example'), 'unauthorized');
  // A public name in Host is final: a client-supplied X-Forwarded-Host cannot override it even through a trusted proxy.
  const injected = { Host: 'kubik.example.com', 'X-Forwarded-Host': 'attacker.example' };
  assert.equal(await verdict(url, injected, 'ca:attacker.example'), 'unauthorized');
  assert.equal(await verdict(url, injected, 'ca:kubik.example.com'), 'welcome');
});

test('X-Forwarded-Host is ignored when the peer is not a trusted proxy (no setting, or another address)', async (t) => {
  for (const trust of [undefined, trusting(), trusting('10.0.0.0/8', '203.0.113.7')]) {
    const { url } = await start(t, { trust });
    const spoofed = { Host: '127.0.0.1:18789', 'X-Forwarded-Host': 'kubik.example.com' };
    assert.equal(await verdict(url, spoofed, 'ca:kubik.example.com'), 'unauthorized', JSON.stringify(trust?.proxies()));
    assert.equal(await verdict(url, spoofed, 'ca:127.0.0.1'), 'welcome');
  }
});

test('a trusted proxy that sends an unusable X-Forwarded-Host gets the connection refused, not a fallback to Host', async (t) => {
  const { url } = await start(t, { trust: trusting('127.0.0.1') });
  for (const bad of ['', ',', 'a b', 'kubik.example.com,', 'ex_ample.com', 'bücher.example']) {
    await assert.rejects(connectDevice(url, { headers: { Host: '127.0.0.1', 'X-Forwarded-Host': bad } }), JSON.stringify(bad));
  }
});

test('hostless ca signatures are rejected regardless of firmware version', async (t) => {
  const { url } = await start(t);
  const host = { Host: 'kubik.example.com' };
  for (const fw of ['0.6.0', '0.5.2', '0.6.1', '1.0.0', undefined, 'dev']) {
    assert.equal(await verdict(url, host, 'ca', fw), 'unauthorized', `fw ${fw}`);
    assert.equal(await verdict(url, host, 'ca:kubik.example.com', fw), 'welcome', `bound fw ${fw}`);
  }
});

test('a signature over ca:<host> is not a plain-ca signature: claiming fw 0.6.0 on another host does not help', async (t) => {
  const { url } = await start(t);
  for (const fw of ['0.6.0', '0.6.1']) {
    assert.equal(await verdict(url, { Host: 'kubik.example.com' }, 'ca:attacker.example', fw), 'unauthorized', `fw ${fw}`);
  }
});

/** Sends raw request-header lines to the route and returns the status line (or 'closed'). */
function rawUpgrade(url, hostLines) {
  return new Promise((resolve) => {
    const socket = connect(Number(new URL(url).port), '127.0.0.1', () => socket.write(['GET /kubik/v1 HTTP/1.1', ...hostLines,
      'Upgrade: websocket', 'Connection: Upgrade', 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==', 'Sec-WebSocket-Version: 13', '', ''].join('\r\n')));
    let text = '';
    socket.on('data', (chunk) => { text += chunk; socket.destroy(); });
    socket.on('close', () => resolve(text.split('\r\n')[0] || 'closed'));
    socket.on('error', () => {});
  });
}

test('a missing, repeated or garbled Host is refused with 400 before any handshake', async (t) => {
  const { url, logs } = await start(t);
  assert.match(await rawUpgrade(url, ['Host: kubik.example.com']), / 101 /);
  for (const lines of [[], ['Host: a.example', 'Host: b.example'], ['Host: a.example, b.example'], ['Host: a b'], ['Host:'], ['Host: :443'],
    ['Host: kubik.example.com:'], ['Host: [::1'], ['Host: ::1'], ['Host: a..example'], ['Host: .'], ['Host: ex_ample.com'], ['Host: a.example/x']]) {
    assert.match(await rawUpgrade(url, lines), /400|closed/, JSON.stringify(lines));
  }
  assert.ok(logs.some((line) => /Host header is missing, repeated or not a plain host name/.test(line)));
});

test('normalizeHostHeader: the exact device-side form, or null', () => {
  const same = [['Example.COM', 'example.com'], ['example.com:443', 'example.com'], ['example.com.', 'example.com'], ['EXAMPLE.com.:1', 'example.com'],
    ['[::1]', '::1'], ['[FE80::1]:8443', 'fe80::1'], ['192.0.2.7:8080', '192.0.2.7'], ['claw', 'claw'], [' kubik.example.com ', 'kubik.example.com'],
    ['xn--bcher-kva.example', 'xn--bcher-kva.example']];
  for (const [value, host] of same) assert.equal(normalizeHostHeader(value), host, value);
  const long = `${'a'.repeat(60)}.${'b'.repeat(60)}.${'c'.repeat(60)}`;
  const bad = [undefined, null, 42, '', ' ', '.', 'a..b', 'a.b..', '-', 'a b', 'a,b', 'a.b, c.d', 'a.b:', ':1', 'a.b:x', 'a.b:123456', '[::1', '::1', '[1.2.3.4]', '[fe80::1%eth0]',
    'ex_ample.com', 'a.b/c', 'a.b?x', 'a@b', 'bücher.example', 'a\tb',`${'a'.repeat(64)}.example`, long, '[]', '[:::' ];
  for (const value of bad) assert.equal(normalizeHostHeader(value), null, String(value));
  assert.equal(normalizeHostHeader('a'.repeat(63) + '.' + 'b'.repeat(59)), `${'a'.repeat(63)}.${'b'.repeat(59)}`); // 123 characters
  assert.equal(normalizeHostHeader('a'.repeat(63) + '.' + 'b'.repeat(60)), null); // 124
});

test('requestBindHost: Host by default; X-Forwarded-Host (last value) only for a trusted peer and an upstream-looking Host', () => {
  const at = (headers, trusted) => requestBindHost(request(headers), trusted);
  assert.deepEqual(at({ host: 'Kubik.example.com:8443' }, false), { host: 'kubik.example.com', header: 'Host' });
  assert.deepEqual(at({ host: '127.0.0.1:18789', 'x-forwarded-host': 'kubik.example.com' }, true), { host: 'kubik.example.com', header: 'X-Forwarded-Host' });
  assert.deepEqual(at({ host: '127.0.0.1:18789', 'x-forwarded-host': 'kubik.example.com' }, false), { host: '127.0.0.1', header: 'Host' });
  assert.deepEqual(at({ host: '[::1]:18789', 'x-forwarded-host': 'a.example,b.example' }, true), { host: 'b.example', header: 'X-Forwarded-Host' });
  assert.deepEqual(at({ host: 'localhost', 'x-forwarded-host': 'kubik.example.com' }, true), { host: 'kubik.example.com', header: 'X-Forwarded-Host' });
  assert.deepEqual(at({ host: 'gw.example.net', 'x-forwarded-host': 'kubik.example.com' }, true), { host: 'gw.example.net', header: 'Host' });
  assert.equal(at({ host: '127.0.0.1', 'x-forwarded-host': 'a b' }, true), null);
  assert.equal(at({}, true), null);
  assert.equal(at({ host: 'a.example, b.example' }, true), null);
  assert.equal(requestBindHost({ headers: { host: 'a.example' }, rawHeaders: ['Host', 'a.example', 'host', 'b.example'] }, false), null);
});
