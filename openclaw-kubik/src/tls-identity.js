// The LAN listener's TLS identity: one P-256 key and a self-signed certificate, created once and kept in the
// plugin state dir. Devices pin the SHA-256 of its SubjectPublicKeyInfo, so the key must survive restarts.
import { createPrivateKey, generateKeyPairSync, randomBytes, sign, X509Certificate } from 'node:crypto';
import { renameSync } from 'node:fs';
import { join } from 'node:path';
import { ensurePrivateDirectory, readPrivateFile, writePrivateFileAtomic } from './private-storage.js';
import { spkiSha256 } from './protocol.js';

export const IDENTITY_FILE = 'lan-tls.json';
const IDENTITY_VERSION = 1;
const MAX_IDENTITY_BYTES = 16 * 1024;
const ECDSA_WITH_SHA256 = Buffer.from('06082a8648ce3d040302', 'hex'); // OID 1.2.840.10045.4.3.2
const COMMON_NAME = Buffer.from('0603550403', 'hex'); // OID 2.5.4.3

/** DER tag-length-value; the contents are concatenated. */
function der(tag, ...contents) {
  const body = Buffer.concat(contents);
  const length = body.length < 0x80 ? Buffer.from([body.length])
    : body.length < 0x100 ? Buffer.from([0x81, body.length]) : Buffer.from([0x82, body.length >> 8, body.length & 0xff]);
  return Buffer.concat([Buffer.from([tag]), length, body]);
}
const sequence = (...contents) => der(0x30, ...contents);

function describe(keyPem, certPem) {
  const key = createPrivateKey(keyPem);
  const x509 = new X509Certificate(certPem);
  if (key.asymmetricKeyDetails?.namedCurve !== 'prime256v1' || !x509.checkPrivateKey(key) || !x509.verify(x509.publicKey)) {
    throw new Error('not a self-signed P-256 identity');
  }
  return { key: keyPem, cert: certPem, spkiHash: spkiSha256(x509.raw) };
}

/**
 * A new P-256 key with a self-signed X.509 v3 certificate (no extensions) valid 1950-01-01 … 9999-12-31, so that a
 * device without a clock accepts it and it never expires. node:crypto cannot create certificates, hence DER.
 */
function generate() {
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const serial = randomBytes(16);
  serial[0] &= 0x7f; // positive INTEGER
  serial[0] |= 0x40; // and minimal (no leading zero byte)
  const algorithm = sequence(ECDSA_WITH_SHA256);
  const name = sequence(der(0x31, sequence(COMMON_NAME, der(0x0c, Buffer.from('Kubik OpenClaw LAN')))));
  const validity = sequence(der(0x17, Buffer.from('500101000000Z')), der(0x18, Buffer.from('99991231235959Z')));
  const tbs = sequence(der(0xa0, der(0x02, Buffer.from([2]))), der(0x02, serial), algorithm, name, validity, name,
    publicKey.export({ type: 'spki', format: 'der' }));
  const signature = sign('sha256', tbs, { key: privateKey, dsaEncoding: 'der' });
  const certificate = sequence(tbs, algorithm, der(0x03, Buffer.from([0]), signature));
  const cert = `-----BEGIN CERTIFICATE-----\n${certificate.toString('base64').match(/.{1,64}/g).join('\n')}\n-----END CERTIFICATE-----\n`;
  return describe(privateKey.export({ type: 'pkcs8', format: 'pem' }), cert);
}

/**
 * Loads the identity from `<dir>/lan-tls.json`, creating it on first use. A file that cannot be parsed is moved
 * aside (`.corrupt-<time>`) and replaced by a new key: pinned devices then report "server key changed". A file
 * that cannot be read (permissions, I/O) throws instead, so a passing fault never changes the key.
 */
export function loadTlsIdentity(dir, { log = () => {} } = {}) {
  ensurePrivateDirectory(dir, 'LAN TLS identity');
  const path = join(dir, IDENTITY_FILE);
  const { text, tooLarge } = readPrivateFile(path, MAX_IDENTITY_BYTES, 'LAN TLS identity');
  if (text !== null) {
    try {
      if (tooLarge) throw new Error('identity file is too large');
      const stored = JSON.parse(text);
      if (stored?.version !== IDENTITY_VERSION) throw new Error('unknown identity format');
      return { ...describe(stored.key, stored.cert), path, created: false };
    } catch {
      const aside = `${path}.corrupt-${Date.now()}`;
      renameSync(path, aside);
      log(`kubik: LAN TLS identity ${path} is unusable; moved to ${aside}, creating a new key`);
    }
  }
  const identity = generate();
  const directorySyncError = writePrivateFileAtomic(path,
    `${JSON.stringify({ version: IDENTITY_VERSION, key: identity.key, cert: identity.cert })}\n`);
  if (directorySyncError) {
    try { log(`kubik: LAN TLS identity committed but its directory could not be synced (${directorySyncError.code ?? 'I/O error'})`); }
    catch { /* the identity rename already committed this key */ }
  }
  log(`kubik: created LAN TLS identity ${path} (key sha256 ${identity.spkiHash.slice(0, 16)}…)`);
  return { ...identity, path, created: true };
}
