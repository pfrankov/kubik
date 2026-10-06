// Gateway route transport: `/kubik/v1` on the OpenClaw Gateway port, so whatever already exposes the Gateway
// (Tailscale Serve/Funnel, Cloudflare Tunnel, a reverse proxy) also serves devices at wss://<host>/kubik/v1.
import { requestBindHost } from './bind-host.js';
import { caBind, DEVICE_PATH } from './protocol.js';

// Shared through globalThis like the server registry: the Gateway may load this module more than once.
const ROUTE_STATE = Symbol.for('openclaw.kubik.route');
const state = globalThis[ROUTE_STATE] ??= { registered: false, error: null };

/** `{ registered, path, error }` for the status surface. */
export const routeStatus = () => ({ registered: state.registered, path: DEVICE_PATH, ...(state.error ? { error: state.error } : {}) });

/**
 * Registers the route (plugin-managed auth: devices prove their key in the protocol). Upgrades go to the running
 * server of the channel's single `default` account (`serverFor()`); none running → 503. TLS ends before the Gateway
 * (a public CA certificate the device validated), so devices sign `ca:<host>` for the host they connected to: the
 * request's `Host`, or `X-Forwarded-Host` from a trusted proxy (see requestBindHost); an unusable host → 400.
 * The client address is the Gateway's own (`gatewayScope().client.clientIp`, resolved through
 * `gateway.trustedProxies`); the socket address otherwise. `trust` = `{ proxies(), isTrusted(ip, proxies) }`
 * (`gateway.trustedProxies` and OpenClaw's matcher for it); without it no proxy is trusted.
 */
export function registerDeviceRoute(api, serverFor, gatewayScope, trust = {}) {
  if (typeof api?.registerHttpRoute !== 'function') {
    state.error = 'this OpenClaw has no plugin HTTP routes';
    return false;
  }
  try {
    api.registerHttpRoute({
      path: DEVICE_PATH, auth: 'plugin', match: 'exact',
      handler: (_req, res) => {
        res.statusCode = 426;
        res.setHeader('Upgrade', 'websocket');
        res.setHeader('Content-Type', 'text/plain');
        res.end('Kubik devices connect here with a WebSocket.\n');
        return true;
      },
      handleUpgrade: (req, socket, head) => {
        const server = serverFor();
        if (!server) { socket.end('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n'); return true; }
        const remote = gatewayScope()?.client?.clientIp ?? req.socket.remoteAddress;
        const peerTrusted = Boolean(trust.isTrusted?.(req.socket.remoteAddress, trust.proxies?.() ?? []));
        const arrival = requestBindHost(req, peerTrusted);
        if (!arrival) {
          server.log(`kubik: refused a gateway-route connection from ${remote}: its Host header is missing, repeated or not a plain host name`);
          socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
          return true;
        }
        server.handleUpgrade(req, socket, head, { via: 'gateway', bind: caBind(arrival.host), remote,
          hint: `this request arrived on host "${arrival.host}" (${arrival.header} header)` });
        return true;
      },
    });
  } catch (error) {
    state.error = error?.message ?? String(error);
    return false;
  }
  Object.assign(state, { registered: true, error: null });
  return true;
}
