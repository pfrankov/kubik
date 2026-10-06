// LAN transport: a TLS WebSocket listener with the plugin's self-signed identity (devices pin its key) and a UDP
// discovery responder. Both serve private peers only, everything else is dropped before TLS or any reply; with
// `publicPeers` (a VPS with a public IP) the TLS listener also serves any peer, while discovery stays private-only.
import { BlockList, isIP } from 'node:net';
import { createSocket } from 'node:dgram';
import { createServer } from 'node:https';
import { networkInterfaces } from 'node:os';
import { DEVICE_PATH, DISCOVERY_REQUEST, LAN_PORT } from './protocol.js';

const MAX_REPLIES_PER_SECOND = 10;
// The server's pre-session limits apply only after the upgrade; these bound the TCP/TLS stage before it.
const LIMITS = { connections: 64, perPeer: 8, handshakeMs: 10_000, requestMs: 10_000 };

const normalized = (address) => {
  const value = String(address ?? '').trim().toLowerCase().replace(/%[^%]*$/, ''); // drop an IPv6 zone (fe80::1%en0)
  const mapped = /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/.exec(value);
  return mapped && isIP(mapped[1]) === 4 ? mapped[1] : isIP(value) ? value : '';
};

// Loopback, RFC 1918, link-local, CGNAT/Tailscale (100.64/10), IPv6 ULA and link-local.
const PRIVATE_NETWORKS = new BlockList();
for (const [network, prefix] of [['127.0.0.0', 8], ['10.0.0.0', 8], ['172.16.0.0', 12], ['192.168.0.0', 16],
  ['169.254.0.0', 16], ['100.64.0.0', 10]]) PRIVATE_NETWORKS.addSubnet(network, prefix, 'ipv4');
for (const [network, prefix] of [['::1', 128], ['fc00::', 7], ['fe80::', 10]]) PRIVATE_NETWORKS.addSubnet(network, prefix, 'ipv6');

/** True for peers the LAN listener may serve (IPv4-mapped IPv6 forms included). */
export function isPrivateAddress(address) {
  const value = normalized(address);
  return Boolean(value) && PRIVATE_NETWORKS.check(value, isIP(value) === 6 ? 'ipv6' : 'ipv4');
}

/** Addresses for the actual listener bind, not unrelated VPN/NICs or loopback. */
export function reachableIPv4Addresses(bound, interfaces, publicPeers = false) {
  return [...new Set(Object.values(interfaces).flat()
    .filter(entry => entry.family === 'IPv4' && !entry.internal &&
      (publicPeers || isPrivateAddress(entry.address)) &&
      (bound === '::' || bound === '0.0.0.0' || entry.address === bound))
    .map(entry => entry.address))].sort();
}

/** Copyable device addresses; they point to Kubik's listener, never the agent API. */
export function deviceServerAddresses(status) {
  return status.addresses.map(address => `kubik://${address}:${status.port}`);
}

function listen(server, port, host) {
  return new Promise((resolve, reject) => {
    const onError = (error) => { server.off('listening', onListening); reject(error); };
    const onListening = () => { server.off('error', onError); resolve(); };
    server.once('error', onError);
    server.once('listening', onListening);
    server.listen({ port, host, ipv6Only: false });
  });
}

export class LanListener {
  #tls = null;
  #udp = null;
  #replies = { second: 0, count: 0 };
  #perPeer = new Map();
  #sockets = new Set(); // every admitted TCP socket, so stop() never waits for an idle one

  /**
   * `host` defaults to `::` (dual stack; falls back to 0.0.0.0 without IPv6). `onUpgrade(req, socket, head, transport)`
   * gets every WebSocket upgrade on /kubik/v1 from a private peer, `transport` = `{ via, bind, remote }` (the SPKI
   * hash a device must have signed). Only the socket address counts as the peer: LAN never reads proxy headers.
   * `isPrivatePeer` (peers that get TLS and discovery replies), `isAllowedPeer` (peers that get TLS: the private ones,
   * or every valid address with `publicPeers`) and `limits` are injectable for tests.
   */
  constructor({ identity, port, host, discoveryPort = LAN_PORT, discoveryHost = '0.0.0.0', onUpgrade, publicPeers = false,
    isPrivatePeer = isPrivateAddress, isAllowedPeer = publicPeers ? (peer) => Boolean(normalized(peer)) : isPrivatePeer,
    limits, log = () => {} }) {
    Object.assign(this, { identity, configuredPort: port, host, discoveryPort, discoveryHost, onUpgrade, publicPeers,
      isPrivatePeer, isAllowedPeer, log });
    this.limits = { ...LIMITS, ...limits };
    this.discoveryError = null;
  }

  get port() { return this.#tls?.address()?.port ?? this.configuredPort; }

  async start() {
    const { handshakeMs, requestMs } = this.limits;
    this.#tls = createServer({ key: this.identity.key, cert: this.identity.cert, minVersion: 'TLSv1.2',
      handshakeTimeout: handshakeMs, headersTimeout: requestMs, requestTimeout: requestMs },
    (_req, res) => { res.statusCode = 404; res.end('Not found\n'); });
    this.#tls.maxConnections = this.limits.connections;
    this.#tls.on('connection', (socket) => this.#admit(socket));
    this.#tls.on('tlsClientError', (_error, socket) => socket.destroy());
    this.#tls.on('clientError', (_error, socket) => socket.destroy());
    this.#tls.on('upgrade', (req, socket, head) => this.#upgrade(req, socket, head));
    await this.#listenTls();
    this.#tls.on('error', (error) => this.log(`kubik: LAN listener error: ${error.code ?? error.message}`));
    await this.#startDiscovery();
    return this.status;
  }

  #upgrade(req, socket, head) {
    const path = req.url.split(/[?#]/)[0].replace(/\/+$/, '');
    if (path !== DEVICE_PATH) {
      socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
      return;
    }
    this.onUpgrade(req, socket, head, { via: 'lan', bind: this.identity.spkiHash, remote: normalized(req.socket.remoteAddress) });
  }

  /** 'connection' is the raw TCP socket, before any TLS byte is answered: private peers only, bounded per peer. */
  #admit(socket) {
    const peer = socket.remoteAddress;
    const count = (this.#perPeer.get(peer) ?? 0) + 1;
    if (!this.isAllowedPeer(peer) || count > this.limits.perPeer) { socket.destroy(); return; }
    this.#perPeer.set(peer, count);
    this.#sockets.add(socket);
    socket.once('close', () => {
      this.#sockets.delete(socket);
      const left = this.#perPeer.get(peer) - 1;
      if (left > 0) this.#perPeer.set(peer, left); else this.#perPeer.delete(peer);
    });
  }

  async #listenTls() {
    if (this.host) { await listen(this.#tls, this.configuredPort, this.host); return; }
    try { await listen(this.#tls, this.configuredPort, '::'); } catch (error) {
      if (!['EAFNOSUPPORT', 'EADDRNOTAVAIL'].includes(error?.code)) throw error;
      await listen(this.#tls, this.configuredPort, '0.0.0.0');
    }
  }

  /** Discovery is best effort: a busy UDP port leaves the listener usable through `kubik://host` on the device. */
  async #startDiscovery() {
    const udp = createSocket({ type: 'udp4' });
    udp.on('message', (message, peer) => this.#answer(udp, message, peer));
    try {
      await new Promise((resolve, reject) => {
        udp.once('error', reject);
        udp.bind(this.discoveryPort, this.discoveryHost, () => { udp.off('error', reject); resolve(); });
      });
    } catch (error) {
      udp.close();
      this.discoveryError = error.code ?? error.message;
      this.log(`kubik: LAN discovery on UDP ${this.discoveryPort} is unavailable (${this.discoveryError}); `
        + 'devices can still use kubik://<this host> as their server');
      return;
    }
    udp.on('error', (error) => this.log(`kubik: LAN discovery error: ${error.code ?? error.message}`));
    this.#udp = udp;
  }

  #answer(udp, message, peer) {
    if (message.toString('latin1') !== DISCOVERY_REQUEST || !peer.port || !this.isPrivatePeer(peer.address)) return; // port 0 makes send() throw
    const second = Math.floor(Date.now() / 1000);
    if (second !== this.#replies.second) this.#replies = { second, count: 0 };
    if (++this.#replies.count > MAX_REPLIES_PER_SECOND) return;
    udp.send(JSON.stringify({ t: 'kubik', v: 5, port: this.port }), peer.port, peer.address);
  }

  /** IPv4 addresses of this host's interfaces a device can reach (private ones; all with `publicPeers`), port, key, discovery. */
  get status() {
    const bound = this.#tls?.address()?.address;
    const addresses = reachableIPv4Addresses(bound, networkInterfaces(), this.publicPeers);
    return { addresses, port: this.port, public: this.publicPeers, spki: this.identity.spkiHash, discovery: this.#udp?.address().port ?? null,
      ...(this.discoveryError ? { discoveryError: this.discoveryError } : {}) };
  }

  async stop() {
    const udp = this.#udp;
    this.#udp = null;
    if (udp) await new Promise((resolve) => udp.close(resolve));
    if (!this.#tls) return;
    for (const socket of this.#sockets) socket.destroy();
    await new Promise((resolve) => this.#tls.close(() => resolve()));
  }
}
