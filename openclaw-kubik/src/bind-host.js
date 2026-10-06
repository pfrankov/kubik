// The host a device connected to, as the gateway route sees it: the `<host>` of the `ca:<host>` transport binding.
// Same normalisation as the device: ASCII lowercase, no port, IPv6 without brackets, no trailing dot; charset [0-9a-z.:-].
import { MAX_BIND_HOST } from './protocol.js';

const HOST_HEADER = /^(?:\[([0-9a-f:.]+)\]|([0-9a-z.-]+))(?::[0-9]{1,5})?$/;
const LABEL = /^[0-9a-z](?:[0-9a-z-]{0,61}[0-9a-z])?$/;

/** The normalised host of one `Host` / `X-Forwarded-Host` value, or null when it is empty, a list or not a plain host[:port]. */
export function normalizeHostHeader(value) {
  if (typeof value !== 'string') return null;
  const match = HOST_HEADER.exec(value.trim().toLowerCase());
  if (!match) return null;
  const [, ipv6, name] = match;
  if (ipv6) return ipv6.includes(':') && ipv6.length <= MAX_BIND_HOST ? ipv6 : null;
  const host = name.endsWith('.') ? name.slice(0, -1) : name;
  return host.length <= MAX_BIND_HOST && host.split('.').every((label) => LABEL.test(label)) ? host : null;
}

// Names a proxy leaves in `Host` when it rewrites it to the upstream: an address or a single-label name.
const isInternalHost = (host) => host.includes(':') || !host.includes('.') || /^[0-9.]+$/.test(host);

/**
 * `{ host, header }` for the host this upgrade request arrived on (`header` says where it came from), or null
 * (refuse: missing, repeated or garbled).
 *
 * `Host` is authoritative. `X-Forwarded-Host` is read only when the socket peer is a trusted proxy (`peerTrusted`,
 * from `gateway.trustedProxies`) and `Host` is one a proxy leaves when it rewrites it to its upstream (an IP
 * address or a single-label name). A public name in `Host` cannot be overridden by a header the client can also
 * send through a proxy that does not overwrite `X-Forwarded-Host`. The last value of a comma list is the one the
 * trusted proxy itself appended; earlier ones came from further out and are not trusted.
 */
export function requestBindHost(req, peerTrusted) {
  const { host: hostHeader, 'x-forwarded-host': forwarded } = req.headers;
  if (req.rawHeaders.filter((name, i) => i % 2 === 0 && name.toLowerCase() === 'host').length !== 1) return null; // Node keeps only the first
  const arrived = normalizeHostHeader(hostHeader);
  if (!arrived) return null;
  if (!peerTrusted || forwarded === undefined || !isInternalHost(arrived)) return { host: arrived, header: 'Host' };
  const host = normalizeHostHeader(forwarded.split(',').at(-1));
  return host && { host, header: 'X-Forwarded-Host' };
}
