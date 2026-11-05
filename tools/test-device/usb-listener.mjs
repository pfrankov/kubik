// Local USB audio fixtures use the same certificate binding as the physical bridge.
import assert from 'node:assert/strict';
import { createPrivateKey, X509Certificate } from 'node:crypto';
import { readFileSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { createServer as createTlsServer } from 'node:https';
import { spkiSha256 } from '../../openclaw-kubik/src/protocol.js';

function tlsIdentity(path) {
  assert.ok(statSync(path).size <= 16 * 1024, 'KUBIK_TEST_TLS_IDENTITY exceeds 16 KB');
  const stored = JSON.parse(readFileSync(path, 'utf8'));
  assert.ok(typeof stored.key === 'string' && typeof stored.cert === 'string',
    'KUBIK_TEST_TLS_IDENTITY requires PEM key and certificate');
  const key = createPrivateKey(stored.key), certificate = new X509Certificate(stored.cert);
  assert.ok(key.asymmetricKeyDetails?.namedCurve === 'prime256v1' && certificate.checkPrivateKey(key) &&
    certificate.verify(certificate.publicKey), 'KUBIK_TEST_TLS_IDENTITY must be a matching self-signed P-256 identity');
  return { key: stored.key, cert: stored.cert, bind: spkiSha256(certificate.raw) };
}

/** No generation or repair: an explicit identity must load exactly or fail before listening. */
export function createUsbListener(options = {}) {
  const { allowPlain = false } = options;
  const identityPath = Object.hasOwn(options, 'identityPath') ? options.identityPath : process.env.KUBIK_TEST_TLS_IDENTITY;
  let http, bind, protocol;
  if (identityPath !== undefined) {
    assert.ok(typeof identityPath === 'string' && identityPath.length > 0, 'KUBIK_TEST_TLS_IDENTITY must name an identity file');
    const identity = tlsIdentity(identityPath);
    http = createTlsServer({ key: identity.key, cert: identity.cert, minVersion: 'TLSv1.2' });
    bind = identity.bind; protocol = 'wss';
  } else {
    assert.ok(allowPlain, 'LAN devices require KUBIK_TEST_TLS_IDENTITY; point the bridge at kubik://127.0.0.1:18999');
    http = createServer(); bind = 'none'; protocol = 'ws';
  }
  const sockets = new Set();
  http.on('request', (_req, res) => { res.writeHead(404); res.end(); });
  http.on('connection', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
  return { http, bind, protocol,
    listen: (port = 18999) => new Promise((resolve, reject) => {
      const failed = error => { http.off('listening', ready); reject(error); };
      const ready = () => { http.off('error', failed); resolve(http.address().port); };
      http.once('error', failed); http.once('listening', ready); http.listen(port, '127.0.0.1');
    }),
    close: () => new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('USB fixture listener did not close within 2 s')), 2000);
      http.close(error => { clearTimeout(timer); if (error && error.code !== 'ERR_SERVER_NOT_RUNNING') reject(error); else resolve(); });
      for (const socket of sockets) socket.destroy();
    }) };
}
