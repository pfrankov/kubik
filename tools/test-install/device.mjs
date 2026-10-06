import { ImaEncoder, KIND_MIC } from '../../openclaw-kubik/src/protocol.js';
// Device simulator that behaves like the firmware: TLS with an SPKI pin (TOFU) or a CA, v5 hello/auth with the
// transport binding it observed (CA route: `ca:<host>`, or plain `ca` on firmware 0.6.0), pairing, playback progress
// and push-to-talk turns with real PCM.
import { createPrivateKey, createPublicKey, generateKeyPairSync, sign } from 'node:crypto';
import WebSocket from 'ws';
import { imaDecode } from '../../openclaw-kubik/test/audio-analysis.js';
import { spkiSha256 } from '../usb-bridge/bridge-protocol.mjs';

const FRAME_BYTES = 1920; // 40 ms of 24 kHz s16le
const PROGRESS_MS = 250;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class PinMismatch extends Error {}

/** A new P-256 device key as PEM (the firmware keeps it in NVS). */
export const newDeviceKeyPem = () => generateKeyPairSync('ec', { namedCurve: 'prime256v1' }).privateKey.export({ format: 'pem', type: 'pkcs8' });

export class SimDevice {
  /** `pin` is the stored SPKI hash (or null before the first successful connection). */
  constructor({ id, keyPem, pin = null, fw = '0.6.2', volume = 70 }) {
    Object.assign(this, { id, pin, fw, volume });
    this.privateKey = createPrivateKey(keyPem);
    const jwk = createPublicKey(this.privateKey).export({ format: 'jwk' });
    this.publicKey = Buffer.concat([Buffer.from([4]), Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]).toString('base64');
    this.reset();
  }

  reset() {
    Object.assign(this, { ws: null, events: [], waiters: new Set(), closeCode: null, observedSpki: null, bind: null, gens: new Map(), welcomed: false });
  }

  /** Opens the socket like the firmware: `ca` validates the server against that CA (the bind names the URL's host), otherwise the leaf must match `pin`. */
  async connect(url, { ca } = {}) {
    this.reset();
    const options = { perMessageDeflate: false, ...(ca ? { ca } : { rejectUnauthorized: false }) };
    const ws = new WebSocket(url, options);
    this.ws = ws;
    ws.on('message', (data, isBinary) => (isBinary ? this.#onAudio(data) : this.#onEvent(JSON.parse(data.toString('utf8')))));
    ws.on('close', (code) => { this.closeCode = code; this.#notify({ t: '__closed__', code }); });
    ws.on('error', () => {});
    await new Promise((resolve, reject) => {
      ws.once('upgrade', (response) => {
        if (ca) { this.bind = this.fw === '0.6.0' ? 'ca' : `ca:${new URL(url).hostname.replace(/^\[|\]$/g, '').replace(/\.$/, '')}`; return; }
        this.observedSpki = spkiSha256(response.socket.getPeerCertificate().raw);
        this.bind = this.observedSpki;
        if (this.pin && this.pin !== this.observedSpki) { ws.terminate(); reject(new PinMismatch(`server key changed: pinned ${this.pin.slice(0, 16)}, saw ${this.observedSpki.slice(0, 16)}`)); }
      });
      ws.once('open', resolve);
      ws.once('error', reject);
    });
    ws.send(JSON.stringify({ t: 'hello', v: 5, device: this.id, key: this.publicKey, fw: this.fw, volume: this.volume, name: 'Install test' }));
  }

  send(message) { this.ws.send(JSON.stringify(message)); }

  /** Resolves the first event at index `from` or later that matches (also ones already received); rejects on timeout. */
  waitFor(match, timeoutMs = 30_000, from = 0) {
    const seen = this.events.slice(from).find(match);
    if (seen) return Promise.resolve(seen);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.waiters.delete(waiter); reject(new Error(`timeout waiting for a device event; got ${this.events.map((e) => (e.code ? `${e.t}(${e.code})` : e.t)).join(',')}`)); }, timeoutMs);
      const waiter = (event) => { if (match(event)) { clearTimeout(timer); this.waiters.delete(waiter); resolve(event); } };
      this.waiters.add(waiter);
    });
  }

  /** First of `pair`, `welcome` or the server closing the socket. */
  outcome(timeoutMs) { return this.waitFor((e) => ['pair', 'welcome', '__closed__'].includes(e.t), timeoutMs); }

  /** The firmware stores the pin only after `welcome`. */
  commitPin() { if (this.welcomed && this.observedSpki) this.pin = this.observedSpki; }

  close() {
    for (const gen of this.gens.values()) clearInterval(gen.timer);
    this.ws?.close(1000);
  }

  #notify(event) { this.events.push(event); for (const waiter of [...this.waiters]) waiter(event); }

  #onEvent(event) {
    if (event.t === 'challenge') {
      const message = Buffer.from(`kubik-auth-v5\n${event.nonce}\n${this.id}\n${this.publicKey}\n${this.bind}`, 'utf8');
      this.send({ t: 'auth', sig: sign('sha256', message, this.privateKey).toString('base64') });
    }
    if (event.t === 'welcome') {
      this.welcomed = true;
      if (Number.isInteger(event.volume) && event.volume >= 0 && event.volume <= 100) this.volume = event.volume;
      this.send({ t: 'device_state', volume: this.volume });
    }
    if (event.t === 'speak') this.gens.set(event.gen, { kind: event.kind, frames: [], pcm: [], pcmBytes: 0, timer: null, startedAt: 0 });
    if (event.t === 'speak_end') this.#finishPlayback(event.gen);
    if (event.t === 'text' && event.receipt) this.send({ t: 'shown', receipt: event.receipt });
    this.#notify(event);
  }

  /** Decodes IMA speech and reports playback progress in real time, as the firmware's audio task does. */
  #onAudio(data) {
    if (data[0] !== 0x03) return;
    const gen = this.gens.get(data[1]);
    if (!gen) return;
    const payload = Buffer.from(data.subarray(2));
    gen.frames.push(payload);
    const pcm = imaDecode(payload);
    gen.pcm.push(pcm);
    gen.pcmBytes += pcm.length;
    if (gen.timer) return;
    gen.startedAt = performance.now();
    gen.timer = setInterval(() => {
      const playedMs = Math.min(Math.round(performance.now() - gen.startedAt), Math.floor(gen.pcmBytes / 48));
      if (this.ws.readyState === 1) this.send({ t: 'progress', gen: data[1], ms: playedMs });
    }, PROGRESS_MS);
  }

  async #finishPlayback(genId) {
    const gen = this.gens.get(genId);
    if (!gen) return;
    const totalMs = gen.pcmBytes / 48;
    await sleep(Math.max(0, gen.startedAt + totalMs - performance.now()));
    clearInterval(gen.timer);
    gen.done = true;
    if (this.ws.readyState === 1) this.send({ t: 'played', gen: genId, ms: Math.round(totalMs) });
  }

  /** One push-to-talk turn with `pcm` (24 kHz s16le) at real-time pace; resolves the reply once the device is idle again. */
  async pushToTalk(pcm, turn = 1) {
    const mark = this.events.length;
    this.send({ t: 'ptt', on: true, turn });
    const encoder = new ImaEncoder();
    const start = performance.now();
    for (let offset = 0, n = 0; offset < pcm.length; offset += FRAME_BYTES, n++) {
      await sleep(Math.max(0, start + n * 40 - performance.now()));
      this.ws.send(Buffer.concat([Buffer.from([KIND_MIC, turn]), encoder.encode(pcm.subarray(offset, offset + FRAME_BYTES))]));
    }
    this.send({ t: 'ptt', on: false, turn, ms: Math.round(performance.now() - start) });
    await this.waitFor((e) => e.t === 'speak_end', 60_000, mark);
    const end = this.events.findIndex((e, i) => i >= mark && e.t === 'speak_end');
    await this.waitFor((e) => e.t === 'state' && e.s === 'idle', 60_000, end + 1);
    return this.replySummary(mark);
  }

  /** What the device received since event index `mark`: states, emotions, and the decoded speech of the reply gen. */
  replySummary(mark = 0) {
    const events = this.events.slice(mark);
    const reply = [...this.gens.values()].find((g) => g.kind === 'reply' && g.frames.length);
    const pcm = reply ? Buffer.concat(reply.pcm) : Buffer.alloc(0);
    let energy = 0;
    for (let i = 0; i + 1 < pcm.length; i += 2) energy += (pcm.readInt16LE(i) / 32768) ** 2;
    return {
      states: events.filter((e) => e.t === 'state').map((e) => e.s),
      emotions: events.filter((e) => e.t === 'emotion').map((e) => e.e),
      frames: reply?.frames.length ?? 0,
      speechMs: Math.round(pcm.length / 48),
      rms: pcm.length ? Number(Math.sqrt(energy / (pcm.length / 2)).toFixed(3)) : 0,
    };
  }
}
