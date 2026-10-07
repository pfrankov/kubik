import { randomBytes, verify } from 'node:crypto';
import { WebSocketServer } from 'ws';
import { isDeviceId, normalizeDeviceId } from './config.js';
import { HttpEngine } from './engines/http.js';
import { authMessage, CLOSE, decodeDeviceKey, out, ProtocolError } from './protocol.js';
import { attachDeviceMessages } from './server-connection.js';
import { guardSessionFrames, readAllowed, withPairingTimeout } from './pairing-access.js';
import { DeviceSession } from './session.js';
import { NotificationError, NotificationQueue } from './notification-queue.js';
import { sanitizeForDisplay, speakableText } from './speech.js';

const HELLO_TIMEOUT_MS = 3000; // until the hello arrives, then again until the auth arrives: a slot is never held longer
const HEARTBEAT_MS = 15_000;
const DEAD_AFTER_MS = 45_000;
const MAX_WS_PAYLOAD = 16 * 1024; // protocol limits (4 KiB JSON / 8 KiB audio) are enforced with close 4002
const PAIRING_POLL_MS = 2000;
const PAIRING_TIMEOUT_MS = 10 * 60_000;
const MAX_PENDING = 4;
const MAX_PENDING_PER_REMOTE = 2;
const MAX_HANDSHAKES = 16;
const MAX_HANDSHAKES_PER_REMOTE = 4;
const BAD_SIGNATURE_CLOSE_DELAY_MS = 300;

function createDefaultEngine(voice, options) {
  if (voice?.provider !== 'openai-http') throw new Error('OpenClaw voice requires an explicit engineFactory');
  return new HttpEngine(voice, options);
}

/** Device sessions of the account, whatever transport accepted the socket. A fresh authenticated connection
 * replaces the old one (close 4003). Keys use v5 challenge–response bound to the transport; unknown keys wait for
 * approval in OpenClaw's pairing store. */
export class KubikServer {
  #wss = null;
  #heartbeat = null;
  #sessions = new Map(); // deviceId -> DeviceSession
  #engines = new Map(); // deviceId -> engine (outlives reconnects)
  #lastGen = new Map();
  #sessionSeq = 0;
  #handshakes = new Map(); // ws -> source until admission transfers it to pairing or a session
  #pending = new Map(); // ws -> connection waiting for pairing approval (or its upsert in flight)
  #poll = null;
  #polling = null;
  #typing = new Map(); // deviceId -> until (ms): a heartbeat run is working on something for this device
  #notifications;
  #notificationDrains = new Map();
  #deferredNotifications = new WeakSet(); // one failed delivery attempt per authenticated connection
  #notificationWaiters = new Map();
  lan = null; // the LAN listener state set by monitor.js: { listener } | { off: true } | { error }

  constructor({ account, dispatch, log = () => {}, setStatus = () => {}, engineFactory = createDefaultEngine, engineOptions = {},
    pairing = null, pairingPollMs = PAIRING_POLL_MS, pairingTimeoutMs = PAIRING_TIMEOUT_MS, activity = null, cron = null,
    notificationPath = null, agentControl = null }) {
    Object.assign(this, { account, dispatch, log, setStatus, engineFactory, engineOptions, pairing, pairingPollMs, pairingTimeoutMs });
    this.cron = cron;
    this.activity = activity;
    this.agentControl = agentControl;
    this.#notifications = new NotificationQueue({ path: notificationPath });
  }

  /** Re-sends what OpenClaw is busy with to every device (called when the Gateway's runs change). */
  refreshActivity() { for (const session of this.#sessions.values()) this.#refreshActivity(session); }

  /**
   * A heartbeat (or a cron job through it) is preparing something for this device: its run lives in another
   * session, so it is shown as the device's own work while the Gateway keeps the typing signal alive.
   */
  setTyping(deviceId, on, ttlMs = 15_000) {
    const id = normalizeDeviceId(deviceId);
    if (on) this.#typing.set(id, Date.now() + ttlMs); else this.#typing.delete(id);
    const session = this.#sessions.get(id);
    if (session) this.#refreshActivity(session);
    if (on) setTimeout(() => { const s = this.#sessions.get(id); if (s) this.#refreshActivity(s); }, ttlMs + 50).unref?.();
  }

  /** Cron jobs changed (the `cron` source's summary, see CronWatcher): tell every device. */
  refreshCron() { for (const session of this.#sessions.values()) this.#sendCron(session, true); }
  #sendCron(session, always = false) {  // a fresh connection starts with nothing shown
    const c = this.cron?.summary();
    if (c && (always || c.running || c.next >= 0)) session.send(out.cron(c.running, c.next));
  }

  #refreshActivity(session) {
    let own = '', other = '';
    if (this.activity) {
      let key;
      try { key = this.activity.sessionKeyFor(session.device.id); } catch { key = undefined; }
      ({ own, other } = this.activity.summary(key));
    }
    const until = this.#typing.get(session.device.id);
    if (until && until < Date.now()) this.#typing.delete(session.device.id);
    else if (until && !own) { own = other || 'thinking'; other = ''; }
    session.setActivity({ own, other });
  }

  get onlineDevices() { return [...this.#sessions.keys()].sort(); }
  getSession(deviceId) { return this.#sessions.get(normalizeDeviceId(deviceId)); }

  /** Offline notifications persist until authenticated device delivery is acknowledged.
   * Online completion distinguishes playback from final text-card installation. */
  async notify(rawDeviceId, text, { signal } = {}) {
    const deviceId = normalizeDeviceId(rawDeviceId);
    if (!isDeviceId(deviceId)) throw new NotificationError('Invalid Kubik device id');
    if (typeof text !== 'string' || (!speakableText(text) && !sanitizeForDisplay(text))) {
      throw new NotificationError('Nothing to say or show in the message');
    }
    signal?.throwIfAborted();
    if (!this.pairing || this.account.devices.get(deviceId)?.enabled === false) throw new NotificationError('Device pairing is unavailable or disabled');
    const allowed = await readAllowed(this.pairing);
    this.#notifications.revoke(allowed);
    const session = this.getSession(deviceId);
    if (session && !allowed.has(`${deviceId}:${session.device.fingerprint}`)) this.#closeRevokedSession(session, 'pairing revoked');
    const keys = [...allowed].filter((entry) => entry.startsWith(`${deviceId}:`)).map((entry) => entry.slice(deviceId.length + 1));
    if (!keys.length) throw new NotificationError('Device is not approved; the message was not accepted');
    signal?.throwIfAborted();
    const entry = this.#notifications.enqueue(deviceId, text, keys);
    if (!this.#onlineSession(deviceId)) return this.#queuedResult(entry);
    const result = new Promise((resolve, reject) => this.#notificationWaiters.set(entry.id, { resolve, reject, entry }));
    this.#drainNotifications(deviceId);
    return result;
  }

  #onlineSession(deviceId) {
    const session = this.getSession(deviceId);
    return session && !session.closed && session.ws.readyState === 1 ? session : null;
  }

  #queuedResult(entry) {
    return { id: entry.id, status: 'queued', queuedAt: entry.createdAt, expiresAt: entry.expiresAt,
      durable: this.#notifications.durable, spokenChars: 0 };
  }

  #drainNotifications(deviceId) {
    if (this.#notificationDrains.has(deviceId)) return;
    const session = this.#onlineSession(deviceId);
    if (!session || this.#deferredNotifications.has(session)) return;
    const drain = (async () => {
      // Recheck on reconnect: only keys approved at enqueue time may receive the queued content.
      this.#notifications.revoke(await readAllowed(this.pairing));
      while (this.#onlineSession(deviceId) === session) {
        const entry = this.#notifications.peek(deviceId, session.device.fingerprint);
        if (!entry) break;
        let attempted = false;
        try {
          const delivered = await session.notify(entry.text, { beforeAttempt: () => {
            attempted = true;
          } });
          if (!attempted) break; // superseded before any output: keep this event for the next connection
          if (['played', 'shown', 'interrupted'].includes(delivered.status)) this.#notifications.remove(entry.id);
          this.#notificationWaiters.get(entry.id)?.resolve({ ...delivered, id: entry.id, queuedAt: entry.createdAt });
          this.#notificationWaiters.delete(entry.id);
        } catch (error) {
          if (!attempted) break; // failed authentication/connection: preserve until another connection
          this.#notificationWaiters.get(entry.id)?.reject(new NotificationError(
            `Kubik device ${deviceId}: ${error?.message ?? 'notification delivery failed'}`, { attempted: true }));
          this.#notificationWaiters.delete(entry.id);
          this.log(`kubik: notification ${entry.id} on ${deviceId} awaits acknowledgement; retained for reconnect`);
          this.#deferredNotifications.add(session);
          break;
        }
      }
    })().catch((error) => {
      this.log(`kubik: cannot deliver queued notifications to ${deviceId}: ${error?.message ?? error}`);
    }).finally(() => {
      this.#notificationDrains.delete(deviceId);
      for (const [id, waiter] of this.#notificationWaiters) {
        if (waiter.entry.deviceId !== deviceId) continue;
        // The queue can also have been purged by revocation. Never claim it is still accepted then.
        if (this.#notifications.has(id)) waiter.resolve(this.#queuedResult(waiter.entry));
        else waiter.reject(new NotificationError('Notification was revoked or expired before delivery'));
        this.#notificationWaiters.delete(id);
      }
      if (this.#onlineSession(deviceId) && this.#onlineSession(deviceId) !== session) this.#drainNotifications(deviceId);
    });
    this.#notificationDrains.set(deviceId, drain);
  }

  /** Starts the connection bookkeeping; transports (LAN listener, Gateway route) hand sockets to `handleUpgrade`. */
  start() {
    this.#wss = new WebSocketServer({ noServer: true, maxPayload: MAX_WS_PAYLOAD, perMessageDeflate: false });
    this.#heartbeat = setInterval(() => this.#checkLiveness(), HEARTBEAT_MS);
    this.#heartbeat.unref?.();
  }

  /**
   * Accepts one device WebSocket upgrade from a transport: `{ via, bind, remote, hint }` = its name, the
   * binding a device must have signed on it (LAN: the listener's SPKI hash, Gateway route: `ca:<host>`), the client
   * address it resolved and a note for the log when the binding does not match.
   */
  handleUpgrade(req, socket, head, transport) {
    if (!this.#wss) { socket.destroy(); return; }
    this.#wss.handleUpgrade(req, socket, head, (ws) => this.#onConnection(ws, transport));
  }

  #checkLiveness() {
    const now = Date.now();
    for (const ws of this.#wss.clients) {
      if (now - ws.kubikLastSeen > DEAD_AFTER_MS) ws.terminate(); else ws.ping();
    }
  }

  #engineFor(device) {
    let engine = this.#engines.get(device.id);
    if (!engine) {
      engine = this.engineFactory(this.account.voice, { log: this.log, ...this.engineOptions });
      this.#engines.set(device.id, engine);
    }
    if (this.agentControl?.voiceSettings) engine.getVoice = () => this.agentControl.voiceSettings(device);
    return engine;
  }

  #onConnection(ws, { via, bind, remote, hint }) {
    ws.on('error', () => {});
    const fromRemote = [...this.#handshakes.values()].filter((address) => address === remote).length;
    if (this.#handshakes.size >= MAX_HANDSHAKES || fromRemote >= MAX_HANDSHAKES_PER_REMOTE) {
      ws.close(CLOSE.PAIRING_BUSY, 'handshake capacity reached'); return;
    }
    this.#handshakes.set(ws, remote);
    ws.kubikLastSeen = Date.now();
    // phase: hello → challenge → checking → pending → session | closed
    const conn = { ws, remote, bind, hint, via, phase: 'hello', session: null, handshakeTimer: null, pendingTimer: null };
    conn.fail = (code, reason) => { conn.phase = 'closed'; ws.close(code, reason); };
    ws.on('close', () => this.#onClose(conn));
    conn.handshakeTimer = setTimeout(() => {
      this.log(`kubik: handshake timeout from ${remote}`);
      conn.fail(CLOSE.PROTOCOL, 'handshake timeout');
    }, HELLO_TIMEOUT_MS);
    attachDeviceMessages(ws, conn, {
      hello: (message) => this.#challenge(conn, message),
      auth: (message) => this.#verifyAuth(conn, message),
      error: (error) => this.#onFrameError(conn, error),
    });
  }

  #onFrameError(conn, error) {
    if (error instanceof ProtocolError) {
      this.log(`kubik: protocol error from ${conn.session?.device.id ?? conn.deviceId ?? conn.remote}: ${error.message}`);
      conn.fail(CLOSE.PROTOCOL, 'protocol error');
      return;
    }
    this.log(`kubik: internal error handling a device frame: ${error?.message ?? error}`);
  }

  #onClose(conn) {
    clearTimeout(conn.handshakeTimer);
    clearTimeout(conn.pendingTimer);
    clearTimeout(conn.rejectTimer);
    this.#handshakes.delete(conn.ws);
    if (this.#pending.delete(conn.ws)) this.#stopPollIfIdle();
    conn.phase = 'closed';
    const { session } = conn;
    if (!session) return;
    session.close();
    if (this.#sessions.get(session.device.id) === session) {
      this.#lastGen.set(session.device.id, session.gen);
      this.#sessions.delete(session.device.id);
      this.log(`kubik: device ${session.device.id} disconnected`);
      this.#publish();
    }
    this.#stopPollIfIdle();
  }

  /** Refuses the device with `unauthorized`; `delayMs` keeps the source slot a moment (bad signatures). */
  #reject(conn, why, delayMs = 0) {
    this.log(`kubik: rejected device ${JSON.stringify(String(conn.deviceId ?? '').slice(0, 64))} from ${conn.remote} via ${conn.via} (${why})`);
    conn.ws.send(JSON.stringify(out.error('unauthorized')));
    if (!delayMs) { conn.fail(CLOSE.UNAUTHORIZED, 'unauthorized'); return; }
    conn.phase = 'closed'; // ignore further auth frames
    conn.rejectTimer = setTimeout(() => conn.fail(CLOSE.UNAUTHORIZED, 'unauthorized'), delayMs);
  }

  /** Step 1: validate the claimed identity and send a fresh nonce. */
  #challenge(conn, hello) {
    const deviceId = normalizeDeviceId(hello.device);
    if (!isDeviceId(deviceId)) throw new ProtocolError('invalid device id');
    conn.deviceId = deviceId;
    const { publicKey, fingerprint } = decodeDeviceKey(hello.key);
    Object.assign(conn, { hello, publicKey, fingerprint, entryId: `${deviceId}:${fingerprint}`,
      nonce: randomBytes(32).toString('base64'), phase: 'challenge' });
    conn.handshakeTimer.refresh(); // the signature gets its own window
    conn.ws.send(JSON.stringify(out.challenge(conn.nonce)));
  }

  /** The device must sign this transport's current binding. */
  #signedBind(conn, auth) {
    const { hello } = conn;
    try {
      return verify('sha256', authMessage({ nonce: conn.nonce, device: hello.device, key: hello.key, bind: conn.bind }),
        { key: conn.publicKey, dsaEncoding: 'der' }, Buffer.from(auth.sig, 'base64'));
    } catch { return false; }
  }

  /** Step 2: check the signature over this transport's binding, then admit, pair or refuse. */
  #verifyAuth(conn, auth) {
    if (!this.#signedBind(conn, auth)) {
      this.#reject(conn, `bad signature or transport binding mismatch: another server key or host, or a relay in between${conn.hint ? `; ${conn.hint}` : ''}`,
        BAD_SIGNATURE_CLOSE_DELAY_MS);
      return;
    }
    clearTimeout(conn.handshakeTimer);
    conn.phase = 'checking';
    this.#admit(conn).catch((error) => {
      this.#pending.delete(conn.ws);
      this.#stopPollIfIdle();
      if (conn.phase === 'closed') return;
      this.log(`kubik: pairing store error for device ${conn.deviceId}: ${error?.message ?? error}`);
      conn.fail(CLOSE.INTERNAL, 'pairing unavailable');
    });
  }

  async #admit(conn) {
    const { deviceId, entryId, remote } = conn;
    if (this.account.devices.get(deviceId)?.enabled === false) { this.#reject(conn, 'device disabled in config'); return; }
    if (!this.pairing) { this.#reject(conn, 'device pairing is unavailable'); return; }
    if ((await readAllowed(this.pairing)).has(entryId)) {
      if (conn.phase === 'checking') await this.#startSession(conn);
      return;
    }
    if (conn.phase !== 'checking') return;
    const waiting = [...this.#pending.values()];
    if (waiting.length >= MAX_PENDING || waiting.filter((c) => c.remote === remote).length >= MAX_PENDING_PER_REMOTE) {
      this.log(`kubik: too many devices waiting for pairing; refused ${deviceId} from ${remote}`);
      conn.fail(CLOSE.PAIRING_BUSY, 'pairing busy');
      return;
    }
    this.#pending.set(conn.ws, conn); // counts against pairing limits while the upsert is in flight
    this.#handshakes.delete(conn.ws); // transfer the slot to the bounded pairing quota
    conn.phase = 'pairing';
    const name = conn.hello.name || deviceId;
    const result = await withPairingTimeout(() => this.pairing.upsert(entryId,
      { senderId: deviceId, name, ...(conn.hello.fw ? { fw: conn.hello.fw } : {}) }), 'request write');
    if (conn.phase !== 'pairing') return; // closed meanwhile
    const code = String(result?.code ?? '').trim().toUpperCase();
    if (!code) {
      this.#pending.delete(conn.ws);
      this.#stopPollIfIdle();
      this.log(`kubik: device ${deviceId} wants to pair, but OpenClaw already has the maximum of pending kubik requests; approve or wait for them to expire (openclaw pairing list kubik)`);
      conn.ws.send(JSON.stringify(out.pair('')));
      conn.fail(CLOSE.PAIRING_BUSY, 'pairing busy');
      return;
    }
    if (result.created) this.log(`kubik: new device ${deviceId} wants to pair — approve with: openclaw pairing approve kubik ${code}`);
    else this.log(`kubik: device ${deviceId} is waiting for pairing approval — approve with: openclaw pairing approve kubik ${code}`);
    conn.phase = 'pending';
    conn.ws.send(JSON.stringify(out.pair(code)));
    conn.pendingTimer = setTimeout(() => {
      if (this.#pending.get(conn.ws) !== conn) return;
      this.#pending.delete(conn.ws);
      this.#stopPollIfIdle();
      this.log(`kubik: device ${deviceId} was not approved in time; closing (it will reconnect with the same code)`);
      conn.fail(CLOSE.PAIRING_TIMEOUT, 'pairing timeout');
    }, this.pairingTimeoutMs);
    conn.pendingTimer.unref?.();
    this.#ensurePairingPoll();
  }

  async #authorizeSession(session) {
    const id = session.device.id;
    if (session.closed || this.#sessions.get(id) !== session) return false;
    let allowed;
    try { allowed = await readAllowed(this.pairing); } catch (error) {
      this.log(`kubik: cannot revalidate device ${id}: ${error?.message ?? error}`);
      this.#closeRevokedSession(session, 'pairing unavailable');
      return false;
    }
    if (session.closed || this.#sessions.get(id) !== session) return false;
    if (allowed.has(`${id}:${session.device.fingerprint}`)) return true;
    this.#closeRevokedSession(session, 'pairing revoked');
    return false;
  }

  #closeRevokedSession(session, reason) {
    if (session.closed || this.#sessions.get(session.device.id) !== session) return;
    this.log(`kubik: device ${session.device.id} ${reason}`);
    session.close();
    session.ws.close(CLOSE.UNAUTHORIZED, reason);
  }

  #ensurePairingPoll() {
    if (!this.pairing || this.#poll || (!this.#pending.size && !this.#sessions.size)) return;
    this.#poll = setInterval(() => { this.checkPairings().catch(() => {}); }, this.pairingPollMs);
    this.#poll.unref?.();
  }

  #stopPollIfIdle() {
    if (this.#pending.size || this.#sessions.size || !this.#poll) return;
    clearInterval(this.#poll);
    this.#poll = null;
  }

  /** Re-reads approved keys for pending and active sessions; `notifyApproval` calls it for Gateway changes. */
  checkPairings() {
    if (this.#polling) return this.#polling;
    if (!this.pairing || (!this.#sessions.size && this.#notifications.empty && ![...this.#pending.values()].some((c) => c.phase === 'pending'))) return Promise.resolve();
    this.#polling = (async () => {
      let allowed;
      try { allowed = await readAllowed(this.pairing); } catch (error) {
        this.log(`kubik: cannot read the pairing allow list: ${error?.message ?? error}`);
        for (const session of [...this.#sessions.values()]) this.#closeRevokedSession(session, 'pairing unavailable');
        return;
      }
      this.#notifications.revoke(allowed);
      for (const session of [...this.#sessions.values()]) {
        if (!allowed.has(`${session.device.id}:${session.device.fingerprint}`)) this.#closeRevokedSession(session, 'pairing revoked');
      }
      for (const conn of [...this.#pending.values()]) {
        if (conn.phase !== 'pending' || !allowed.has(conn.entryId)) continue;
        this.#pending.delete(conn.ws);
        clearTimeout(conn.pendingTimer);
        this.log(`kubik: device ${conn.deviceId} approved`);
        await this.#startSession(conn);
      }
      this.#stopPollIfIdle();
    })().finally(() => { this.#polling = null; });
    return this.#polling;
  }

  async #startSession(conn) {
    const { ws, remote, hello, deviceId } = conn;
    const configured = this.account.devices.get(deviceId);
    const device = { id: deviceId, name: configured && configured.name !== deviceId ? configured.name : hello.name || deviceId,
      enabled: true, fingerprint: conn.fingerprint, fw: hello.fw, reportedName: hello.name,
      ...(hello.volume !== undefined ? { volume: hello.volume } : {}) };
    const engine = this.#engineFor(device);
    try { await engine.refreshCapabilities?.({ agentId: this.agentControl?.agentId?.(device) }); }
    catch (error) { this.log(`kubik: cannot refresh agent voice capabilities for ${deviceId}: ${error?.message ?? error}`); }
    if (conn.phase === 'closed' || ws.readyState !== 1) return;
    // Commit replacement without an await: another authenticated connection may
    // have become current while capabilities were being refreshed.
    const previous = this.#sessions.get(deviceId);
    if (previous) {
      this.log(`kubik: device ${deviceId} reconnected; replacing the previous connection`);
      this.#lastGen.set(deviceId, previous.gen);
      previous.close();
      previous.ws.close(CLOSE.REPLACED, 'replaced');
    }
    const session = new DeviceSession({
      ws, device, engine, log: this.log,
      lastGen: this.#lastGen.get(deviceId) ?? 0, sessionId: `s-${++this.#sessionSeq}`, volume: this.account.volume ?? hello.volume,
      agentControl: this.agentControl,
      textMode: this.account.text ?? 'auto',
      onActivity: () => this.setStatus({ lastInboundAt: Date.now() }),
      authorize: () => this.#authorizeSession(session),
      dispatch: async (turn) => {
        if (!turn.isCurrent?.() || !await this.#authorizeSession(session)) return;
        if (session.closed || this.#sessions.get(deviceId) !== session || !turn.isCurrent?.()) return;
        return this.dispatch({ device, ...turn, speak: turn.deliver });
      },
      current: () => this.#sessions.get(deviceId),
    });
    conn.session = session;
    conn.phase = 'session';
    conn.frames = guardSessionFrames(session, conn, { authorize: () => this.#authorizeSession(session),
      onFailure: (error) => {
        if (error) this.log(`kubik: pairing check failed for device ${deviceId}: ${error?.message ?? error}`);
        this.#closeRevokedSession(session, error ? 'pairing unavailable' : 'pairing busy');
      },
      onFrameError: (error) => this.#onFrameError(conn, error) });
    this.#sessions.set(deviceId, session);
    this.#handshakes.delete(ws); // transfer the slot to the live session
    this.#ensurePairingPoll();
    session.start();
    this.#refreshActivity(session);
    this.#sendCron(session);
    this.log(`kubik: device ${deviceId} connected from ${remote} via ${conn.via}${hello.fw ? ` (fw ${hello.fw})` : ''} (key ${conn.fingerprint.slice(0, 8)})`);
    this.#publish();
    this.#drainNotifications(deviceId);
  }

  #publish() {
    this.setStatus({ onlineDevices: this.onlineDevices, lastDeviceChangeAt: Date.now() });
  }

  async stop() {
    clearInterval(this.#heartbeat);
    clearInterval(this.#poll);
    this.#poll = null;
    await this.lan?.listener?.stop();
    for (const conn of this.#pending.values()) clearTimeout(conn.pendingTimer);
    this.#pending.clear();
    for (const session of this.#sessions.values()) session.close();
    this.#sessions.clear();
    const clients = [...this.#wss?.clients ?? []];
    for (const ws of clients) ws.close(CLOSE.GOING_AWAY, 'server stopping');
    for (const engine of this.#engines.values()) { try { engine.close(); } catch { /* best effort */ } }
    this.#engines.clear();
    setTimeout(() => clients.forEach((ws) => ws.terminate()), 1000).unref?.();
    await new Promise((resolve) => this.#wss ? this.#wss.close(() => resolve()) : resolve());
    this.#wss = null;
  }
}
