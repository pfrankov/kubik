import WebSocket from 'ws';
import { randomInt } from 'node:crypto';
import { F, routedPayload, serverTarget, spkiSha256 } from './bridge-protocol.mjs';
import { discoverServer } from './lan-discovery.mjs';
import { CLOSE } from '../../openclaw-kubik/src/protocol.js';

const { REPLACED } = CLOSE;

function parseJson(data) {
  try { return JSON.parse(data.toString('utf8')); } catch { return null; }
}

function isWelcome(message) {
  return message?.t === 'welcome' && typeof message.session === 'string' && message.session.length > 0;
}

function isHandshake(message) {
  return message?.t === 'challenge' || message?.t === 'pair';
}

export class UsbRoute {
  constructor({ server, enabled = true, send, log = () => {}, socketFactory = (url, cfg) => new WebSocket(url, cfg),
    discover = discoverServer, setTimer = setTimeout, clearTimer = clearTimeout, epoch = randomInt(1, 0xffffffff) }) {
    Object.assign(this, { server, enabled, send, log, socketFactory, discover, setTimer, clearTimer, epoch });
    this.attached = false; this.status = 'disabled'; this.ws = null; this.retry = 0;
    this.hello = null; this.welcome = null; this.handshake = null;
  }
  announce() {
    if (this.attached) this.send(F.HOST_HELLO, Buffer.from(JSON.stringify({ v: 2, status: this.status, epoch: this.epoch })));
  }
  attach() { this.attached = true; this.retry = 0; this.#probe(); }
  detach() { this.attached = false; this.#clear(); this.hello = this.welcome = this.handshake = null; }
  setEnabled(enabled) {
    if (typeof enabled !== 'boolean') throw new TypeError('enabled must be a boolean');
    if (enabled === this.enabled) return;
    this.enabled = enabled; this.retry = 0; this.#probe();
  }
  #clear() {
    for (const key of ['authTimer', 'retryTimer', 'healthTimer', 'pongTimer']) {
      if (this[key]) this.clearTimer(this[key]); this[key] = null;
    }
    if (this.ws) {
      const old = this.ws; this.ws = null;
      old.removeAllListeners(); old.on('error', () => {}); old.terminate();
    }
  }
  #probe() {
    this.#clear();
    this.epoch = (this.epoch + 1) >>> 0 || 1;
    this.hello = this.welcome = this.handshake = null;
    this.status = this.enabled ? 'probing' : 'disabled';
    this.announce();
  }
  /** Standby costs nothing: `probing` only listens for the device's hello, which is its demand for USB. */
  #fail(reason) {
    if (!this.attached || !this.enabled) return;
    this.#clear(); this.status = 'unavailable'; this.welcome = this.handshake = null;
    this.announce();
    const delay = [2000, 5000, 10000, 30000][Math.min(this.retry++, 3)];
    this.log(`USB upstream unavailable (${reason}); retry in ${delay / 1000}s`);
    this.retryTimer = this.setTimer(() => this.#probe(), delay);
  }
  /** The server replaced this session: the device came back online through Wi-Fi, so USB returns to standby. */
  #release() {
    this.log('USB upstream released (the device is online through another route)');
    this.retry = 0; this.#probe();
  }
  #health() {
    this.healthTimer = this.setTimer(() => {
      const sock = this.ws;
      if (!sock || this.status !== 'ready') return;
      this.pongTimer = this.setTimer(() => this.#fail('pong timeout'), 3000);
      try { sock.ping(); } catch { this.#fail('ping failed'); }
    }, 5000);
  }
  /**
   * The bridge is the TLS client, so it reports the transport binding it observed to the device (frame 0x23) before
   * the hello goes upstream, hence before any challenge: `ca:<host>`, `none`, or the LAN server's SPKI hash, which the
   * device checks against its pin and signs exactly as on a direct connection.
   */
  #connect(target) {
    const pinned = target.bind === 'spki';
    const sock = this.socketFactory(target.url, { perMessageDeflate: false, handshakeTimeout: 6000, maxPayload: 6144,
      ...(pinned ? { rejectUnauthorized: false } : {}) });
    this.ws = sock; sock.binaryType = 'nodebuffer';
    let bind = pinned ? null : target.bind;
    sock.on('upgrade', (response) => {
      if (!pinned) return;
      try { bind = spkiSha256(response.socket.getPeerCertificate(true).raw); } catch { bind = null; }
    });
    sock.on('open', () => {
      if (this.ws !== sock) return;
      if (!bind) { this.#fail('LAN server presented no usable certificate'); return; }
      this.send(F.BIND, routedPayload(this.epoch, bind));
      sock.send(this.serverHello); // exactly one hello per server connection
    });
    sock.on('message', (data, binary) => this.#handleMessage(sock, data, binary));
    sock.on('pong', () => this.#handlePong(sock));
    sock.on('close', (code) => {
      if (this.ws !== sock) return;
      if (code === REPLACED) this.#release(); else this.#fail(`closed ${code}`);
    });
    sock.on('error', (err) => { if (this.ws === sock) this.#fail(err.message); });
  }
  #handleMessage(sock, data, binary) {
    if (this.ws !== sock) return;
    const message = binary ? null : parseJson(data);
    if (isWelcome(message)) return this.#acceptWelcome(data);
    if (this.status === 'ready') return this.#relayServerPayload(data, binary);
    if (!binary && isHandshake(message)) this.#acceptHandshake(data, message);
  }
  #acceptWelcome(data) {
    this.clearTimer(this.authTimer); this.authTimer = null;
    this.welcome = Buffer.from(data); this.handshake = null; this.status = 'ready'; this.retry = 0;
    // FIFO serial writes guarantee readiness precedes its matching welcome.
    this.announce();
    this.send(F.JSON, routedPayload(this.epoch, data));
    if (this.healthTimer) this.clearTimer(this.healthTimer);
    this.#health();
    this.log('USB upstream authenticated');
  }
  #relayServerPayload(data, binary) {
    this.send(binary ? F.AUDIO : F.JSON, routedPayload(this.epoch, data));
  }
  #acceptHandshake(data, message) {
    // Keep challenge or pairing messages until answered so a repeated hello can be answered again.
    this.handshake = Buffer.from(data);
    if (message.t === 'pair' && message.code) {
      this.clearTimer(this.authTimer); this.authTimer = null;
      this.log(`device waits for pairing: openclaw pairing approve kubik ${message.code}`);
    }
    this.send(F.JSON, routedPayload(this.epoch, data));
  }
  #handlePong(sock) {
    if (this.ws !== sock || this.status !== 'ready') return;
    if (this.pongTimer) this.clearTimer(this.pongTimer);
    this.pongTimer = null;
    if (this.healthTimer) this.clearTimer(this.healthTimer);
    this.#health();
  }
  deviceFrame(type, payload) {
    if (!this.enabled || !this.attached || payload.length < 4 || payload.readUInt32LE(0) !== this.epoch) return null;
    const data = payload.subarray(4);
    if (type === F.JSON) {
      const msg = parseJson(data);
      if (msg?.t === 'hello') return this.#handleHello(msg, data);
      if (msg?.t === 'auth') return this.#handleAuth(data);
    }
    if (this.status !== 'ready' || !this.ws || this.ws.readyState !== WebSocket.OPEN) return null;
    this.ws.send(data, { binary: type === F.AUDIO });
    return data;
  }
  #handleHello(message, data) {
    const hello = data.toString('utf8');
    if (this.hello !== null && this.hello !== hello) { this.#probe(); return null; }
    if (this.status === 'ready' && this.welcome) {
      this.announce(); this.send(F.JSON, routedPayload(this.epoch, this.welcome));
    } else if (this.status === 'probing' && this.ws && this.handshake) {
      this.send(F.JSON, routedPayload(this.epoch, this.handshake));
    } else if (this.status === 'probing' && !this.ws && this.hello === null) {
      this.#startConnection(message, hello);
    }
    return null;
  }
  #startConnection(message, hello) {
    this.hello = hello;
    const { server: configuredServer, ...serverHello } = message;
    let target;
    try { target = serverTarget(this.server ?? configuredServer); }
    catch (err) { this.#fail(err.message); return; }
    this.serverHello = JSON.stringify(serverHello);
    this.authTimer = this.setTimer(() => this.#fail('authentication timeout'), 8000);
    this.log(`device hello: ${message.device} fw ${message.fw} (key authentication)`);
    if (!target.discover) { this.#open(target); return; }
    const epoch = this.epoch;
    this.discover().then((url) => {
      if (this.epoch !== epoch || !this.attached || this.status !== 'probing' || this.ws) return;
      this.log(`LAN discovery: ${url}`);
      this.#open({ url, bind: target.bind });
    }, (err) => { if (this.epoch === epoch) this.#fail(err.message); });
  }
  #open(target) {
    try { this.#connect(target); } catch (err) { this.#fail(err.message); }
  }
  #handleAuth(data) {
    // The device's signed answer to the relayed challenge of this server connection.
    if (this.status !== 'probing' || !this.ws || this.ws.readyState !== WebSocket.OPEN || !this.handshake) return null;
    this.handshake = null;
    this.ws.send(data, { binary: false });
    return data;
  }
}
