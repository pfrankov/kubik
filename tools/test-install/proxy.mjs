// TLS pieces for the Gateway-route and MITM cases: a throwaway CA (openssl) with a 127.0.0.1 server certificate,
// and a TLS-terminating TCP forwarder that stands in for Tailscale/Cloudflare (route case) or an attacker (relay case).
import { execFile } from 'node:child_process';
import { readFile, writeFile } from 'node:fs/promises';
import { connect as netConnect } from 'node:net';
import { join } from 'node:path';
import { createServer as createTlsServer, connect as tlsConnect } from 'node:tls';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const KEY = ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes'];

/** CA plus a server certificate for 127.0.0.1/localhost signed by it; returns PEM strings. */
export async function createCa(dir) {
  const file = (name) => join(dir, name);
  const openssl = (...args) => execFileAsync('openssl', args, { cwd: dir }).catch((error) => { throw new Error(`openssl ${args[0]} failed: ${error.stderr || error.message}`); });
  await writeFile(file('ca.cnf'), '[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=Kubik install test CA\n[ext]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n');
  await writeFile(file('leaf.cnf'), 'subjectAltName=IP:127.0.0.1,DNS:localhost\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n');
  await openssl('req', '-x509', ...KEY, '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-config', 'ca.cnf');
  await openssl('req', '-new', ...KEY, '-keyout', 'leaf.key', '-out', 'leaf.csr', '-subj', '/CN=127.0.0.1');
  await openssl('x509', '-req', '-in', 'leaf.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'leaf.pem', '-days', '2', '-extfile', 'leaf.cnf');
  const [ca, cert, key] = await Promise.all(['ca.pem', 'leaf.pem', 'leaf.key'].map((name) => readFile(file(name), 'utf8')));
  return { ca, cert, key };
}

/** The first request with `X-Forwarded-For` / `X-Forwarded-Host` added and `Host` replaced, as the options ask. */
function rewriteHead(head, { forwardedFor, forwardedHost, host }) {
  const [requestLine, ...rest] = head.toString('latin1').split('\r\n');
  const added = [forwardedFor && `X-Forwarded-For: ${forwardedFor}`, forwardedHost && `X-Forwarded-Host: ${forwardedHost}`].filter(Boolean);
  const headers = host ? rest.map((line) => (/^host:/i.test(line) ? `Host: ${host}` : line)) : rest;
  return Buffer.from([requestLine, ...added, ...headers].join('\r\n'), 'latin1');
}

/**
 * Listens with TLS on loopback and pipes every connection to `upstream` (`{ port, tls }`): plain TCP to the Gateway
 * for a reverse proxy, or a fresh unverified TLS session to the LAN listener for a relaying attacker. In the first
 * request `forwardedFor` / `forwardedHost` add `X-Forwarded-For` / `X-Forwarded-Host` headers, like a reverse proxy
 * naming the client and the host it serves, and `host` replaces the `Host` header (a proxy that talks to its
 * upstream by address, or a relay claiming another host).
 */
export async function startForwarder({ cert, key }, upstream) {
  const sockets = new Set();
  const rewrites = Boolean(upstream.forwardedFor || upstream.forwardedHost || upstream.host);
  const server = createTlsServer({ cert, key }, (client) => {
    const target = upstream.tls
      ? tlsConnect({ host: '127.0.0.1', port: upstream.port, rejectUnauthorized: false })
      : netConnect({ host: '127.0.0.1', port: upstream.port });
    for (const socket of [client, target]) { sockets.add(socket); socket.on('error', () => socket.destroy()); socket.on('close', () => { sockets.delete(socket); (socket === client ? target : client).destroy(); }); }
    if (rewrites) client.once('data', (head) => { target.write(rewriteHead(head, upstream)); client.pipe(target); });
    else client.pipe(target);
    target.pipe(client);
  });
  server.on('tlsClientError', () => {});
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    port: server.address().port,
    close: async () => { for (const socket of sockets) socket.destroy(); await new Promise((resolve) => server.close(resolve)); },
  };
}
