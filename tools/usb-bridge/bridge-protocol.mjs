import { DEVICE_PATH, LAN_PORT } from '../../openclaw-kubik/src/protocol.js';

export { spkiSha256 } from '../../openclaw-kubik/src/protocol.js';

export const F = { JSON: 0x01, AUDIO: 0x02, LOG: 0x10, HOST_HELLO: 0x20, CONFIG: 0x21, CONFIG_REPLY: 0x22, BIND: 0x23 };
const MAX_URL_BYTES = 127;

// v2 routed frames carry the current bridge epoch before their server payload.
// Config, replies and logs retain their original framing when routing is disabled.
export function routedPayload(epoch, data) {
  const payload = Buffer.isBuffer(data) ? data : Buffer.from(data);
  const routed = Buffer.allocUnsafe(payload.length + 4);
  routed.writeUInt32LE(epoch, 0);
  payload.copy(routed, 4);
  return routed;
}

function invalidServerUrl() {
  return new Error('invalid server URL: use ws:// or wss:// with hostname and absolute path, without credentials, query or fragment');
}

function hasWebSocketUrlShape(value) {
  return value.length <= MAX_URL_BYTES && !/[^\x21-\x7e]|[\\@?#]/.test(value) && /^wss?:\/\/[^/]+\//.test(value);
}

function parseWebSocketUrl(value) {
  try { return new URL(value); } catch { throw invalidServerUrl(); }
}

function hasValidAuthority(url) {
  return url.hostname && !url.username && !url.password && !url.hash && !url.search && url.port !== '0';
}

function isValidHostname(hostname) {
  if (hostname.startsWith('[')) return true;
  return hostname.split('.').every((part) => /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/i.test(part));
}

export function serverUrl(value) {
  if (typeof value !== 'string' || !hasWebSocketUrlShape(value)) throw invalidServerUrl();
  const url = parseWebSocketUrl(value);
  if (!hasValidAuthority(url) || !isValidHostname(url.hostname)) throw invalidServerUrl();
  return value;
}

function lanTarget(value) {
  const invalid = () => new Error('invalid server: kubik://host[:port] has no path, credentials, query or fragment');
  if (value.length > MAX_URL_BYTES || !/^kubik:\/\/[^/\\@?#\s]+\/?$/.test(value)) throw invalid();
  let url;
  try { url = new URL(`https://${value.slice('kubik://'.length)}`); } catch { throw invalid(); }
  if (!url.hostname || url.port === '0' || !isValidHostname(url.hostname)) throw invalid();
  return { url: `wss://${url.hostname}:${url.port || LAN_PORT}${DEVICE_PATH}`, bind: 'spki' };
}

/** The `ca:<host>` binding of a `wss://` URL, from its text as the firmware reads it: lowercase host, no port or IPv6 brackets. */
function caBind(value) {
  const authority = value.slice('wss://'.length).split('/')[0];
  return `ca:${(authority.startsWith('[') ? authority.slice(1, authority.indexOf(']')) : authority.split(':')[0]).toLowerCase()}`;
}

/**
 * Where the device's server setting (or --server) points and which transport binding the device will sign:
 * empty → LAN discovery then TLS pinned by the device (`spki`), `kubik://host[:port]` → the same without
 * discovery, `wss://` → public CA and the host it dials (`ca:<host>`), `ws://` → development (`none`).
 */
export function serverTarget(value) {
  if (value === undefined || value === null || value === '') return { discover: true, bind: 'spki' };
  if (typeof value === 'string' && value.startsWith('kubik://')) return lanTarget(value);
  return { url: serverUrl(value), bind: value.startsWith('wss://') ? caBind(value) : 'none' };
}
