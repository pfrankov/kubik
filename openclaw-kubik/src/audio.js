import { BYTES_PER_MS, MAX_SPEECH_PAYLOAD_BYTES, SAMPLE_RATE, SPEECH_LEAD_MS } from './protocol.js';

/** Wraps s16le mono PCM in a RIFF/WAVE header. */
export function pcmToWav(pcm, sampleRate = SAMPLE_RATE) {
  const header = Buffer.alloc(44);
  header.write('RIFF', 0); header.writeUInt32LE(36 + pcm.length, 4); header.write('WAVE', 8);
  header.write('fmt ', 12); header.writeUInt32LE(16, 16); header.writeUInt16LE(1, 20); header.writeUInt16LE(1, 22);
  header.writeUInt32LE(sampleRate, 24); header.writeUInt32LE(sampleRate * 2, 28); header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34); header.write('data', 36); header.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([header, pcm]);
}

/** Decodes G.711 mu-law bytes to s16le PCM. */
export function mulawToPcm16(data) {
  const pcm = Buffer.alloc(data.length * 2);
  for (let i = 0; i < data.length; i++) {
    const u = ~data[i] & 0xff;
    const exponent = (u >> 4) & 0x07;
    let sample = ((((u & 0x0f) << 3) + 0x84) << exponent) - 0x84;
    if (u & 0x80) sample = -sample;
    pcm.writeInt16LE(sample, i * 2);
  }
  return pcm;
}

/** Linear-interpolation resampler for s16le mono PCM (speech quality; used for provider rates != 24 kHz). */
export function resamplePcm16(pcm, fromRate, toRate = SAMPLE_RATE) {
  if (!Number.isFinite(fromRate) || fromRate <= 0) throw new RangeError('invalid sample rate');
  const input = Math.floor(pcm.length / 2);
  if (fromRate === toRate || input === 0) return pcm.subarray(0, input * 2);
  const output = Math.max(1, Math.round((input * toRate) / fromRate));
  const out = Buffer.alloc(output * 2);
  const step = fromRate / toRate;
  for (let i = 0; i < output; i++) {
    const position = i * step;
    const index = Math.floor(position);
    const a = pcm.readInt16LE(Math.min(index, input - 1) * 2);
    const b = pcm.readInt16LE(Math.min(index + 1, input - 1) * 2);
    out.writeInt16LE(Math.round(a + (b - a) * (position - index)), i * 2);
  }
  return out;
}

/** Strips a RIFF/WAVE header when present; returns { pcm, sampleRate } for 16-bit mono PCM WAV. */
export function parseWav(buffer) {
  if (buffer.length < 12 || buffer.toString('ascii', 0, 4) !== 'RIFF' || buffer.toString('ascii', 8, 12) !== 'WAVE') return null;
  let offset = 12;
  let format = null;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString('ascii', offset, offset + 4);
    const size = buffer.readUInt32LE(offset + 4);
    const body = offset + 8;
    if (id === 'fmt ') format = { tag: buffer.readUInt16LE(body), channels: buffer.readUInt16LE(body + 2), sampleRate: buffer.readUInt32LE(body + 4), bits: buffer.readUInt16LE(body + 14) };
    if (id === 'data') {
      if (!format || format.tag !== 1 || format.channels !== 1 || format.bits !== 16) throw new RangeError('only 16-bit mono PCM WAV is supported');
      return { pcm: buffer.subarray(body, Math.min(buffer.length, body + size)), sampleRate: format.sampleRate };
    }
    offset = body + size + (size % 2);
  }
  throw new RangeError('WAV has no data chunk');
}

/** Keeps 16-bit sample alignment across arbitrarily split network chunks. */
export class SampleAligner {
  #carry = null;
  push(chunk) {
    let data = this.#carry ? Buffer.concat([this.#carry, chunk]) : chunk;
    this.#carry = null;
    if (data.length % 2) { this.#carry = data.subarray(data.length - 1); data = data.subarray(0, data.length - 1); }
    return data;
  }
  reset() { this.#carry = null; }
}

/**
 * Device consumption alone releases send credit, bounding queued and in-flight audio together.
 * Wall-clock playback estimates are only for emotion markers and delivery deadlines, never pacing.
 * Markers run when preceding audio has been handed to the socket.
 */
export class SpeechPacer {
  #items = [];
  #running = false;
  #wake = null;
  #drainWaiters = [];
  #playEnd = 0;
  #prebufLeft = 0; // ms the device still collects before it starts playing the current gen
  #held = 0; // ms sent for the current gen while the device was still collecting
  #confirmed = 0;
  #progressAt = 0;
  #sent = 0; // ms sent for the current gen
  #trace = null;
  #finishWait = null;
  #partialSince = null;
  constructor({ sendAudio, prebufMs = 400, frameBytes = MAX_SPEECH_PAYLOAD_BYTES, bufferedAmount = () => 0,
    maxBufferedBytes = 256 * 1024, progressWindowMs = SPEECH_LEAD_MS, onStall = () => {}, onTrace = () => {}, now = () => performance.now() }) {
    Object.assign(this, { sendAudio, prebufMs, frameBytes, bufferedAmount, maxBufferedBytes, progressWindowMs, onStall, onTrace, now });
  }
  /** The device starts after prebuffering; a new generation owns fresh credit and marker estimates. */
  startGen(trace = null) {
    this.#trace = trace; this.#prebufLeft = this.prebufMs; this.#held = 0;
    this.#sent = this.#confirmed = 0; this.#playEnd = this.#progressAt = this.now();
  }
  /** Only monotonic consumption within the sent duration grants credit, including network transit. */
  played(ms) {
    const unplayed = this.#sent - ms;
    if (!(unplayed >= 0) || ms < this.#confirmed) return;
    if (ms > this.#confirmed) this.#progressAt = this.now();
    this.#confirmed = ms;
    this.#trace?.progress(ms);
    this.#wake?.();
    this.#playEnd = this.now() + unplayed;
  }
  get sentMs() { return this.#sent; }
  finishTrace(outcome) {
    this.#finishWait?.();
    const trace = this.#trace; this.#trace = null;
    try { if (trace) this.onTrace(outcome, trace.summary()); } catch { /* diagnostics never interrupt playback or cancellation */ }
  }
  get queuedBytes() { return this.#items.reduce((sum, item) => sum + (item.pcm ? item.pcm.length : 0), 0); }
  get idle() { return !this.#items.length && !this.#running; }
  /** Milliseconds of already-sent audio still to be played by the device (estimate). */
  get aheadOfRealtimeMs() { return Math.max(0, this.#playEnd - this.now()); }
  push(pcm) {
    if (!pcm?.length) return;
    this.#items.push({ pcm });
    if (this.#partialSince !== null) this.#wake?.();
    this.#pump();
  }
  mark(fn) { this.#items.push({ marker: fn }); this.#wake?.(); this.#pump(); }
  /** Drops everything not yet sent. Device-side audio for the old gen is dropped by the device itself. */
  clear() {
    this.#items = [];
    this.#prebufLeft = 0;
    this.#partialSince = null;
    this.#playEnd = this.now();
    this.#wake?.();
  }
  drained() { return this.idle ? Promise.resolve() : new Promise((resolve) => this.#drainWaiters.push(resolve)); }
  #sleep(ms, kind) {
    const trace = this.#trace, started = this.now();
    let settled = false;
    const settle = () => { if (!settled) { settled = true; trace?.waited(kind, this.now() - started); } };
    this.#finishWait = settle;
    return new Promise((resolve) => {
      const timer = setTimeout(done, ms);
      function done() { clearTimeout(timer); resolve(); }
      this.#wake = done;
    }).finally(() => { this.#wake = null; settle(); if (this.#finishWait === settle) this.#finishWait = null; });
  }
  #awaitingProgress(frameMs) {
    if (this.#sent - this.#confirmed + frameMs <= this.progressWindowMs) return false;
    if (this.now() - this.#progressAt >= 5000) { this.clear(); this.onStall(); }
    return true;
  }
  #nextFrame() {
    let size = 0;
    for (const item of this.#items) {
      if (item.marker) return { size, wait: 0 };
      size += item.pcm.length;
      if (size >= this.frameBytes) return { size: this.frameBytes, wait: 0 };
    }
    this.#partialSince ??= this.now();
    return { size, wait: Math.max(0, this.#partialSince + 100 - this.now()) };
  }
  #takeFrame(size) {
    const parts = [];
    for (let left = size; left > 0;) {
      const item = this.#items[0], take = Math.min(item.pcm.length, left);
      parts.push(item.pcm.subarray(0, take)); left -= take;
      if (take === item.pcm.length) this.#items.shift(); else item.pcm = item.pcm.subarray(take);
    }
    this.#partialSince = null;
    return parts.length === 1 ? parts[0] : Buffer.concat(parts, size);
  }
  async #pump() {
    if (this.#running) return;
    this.#running = true;
    try {
      while (this.#items.length) {
        const head = this.#items[0];
        if (head.marker) {
          this.#items.shift();
          try { head.marker(Math.max(this.now(), this.#playEnd)); } catch { /* marker errors never stop audio */ }
          continue;
        }
        const { size, wait } = this.#nextFrame();
        if (wait) { await this.#sleep(wait, 'batch'); continue; }
        if (this.#awaitingProgress(size / BYTES_PER_MS)) { await this.#sleep(20, 'credit'); continue; }
        const buffered = this.bufferedAmount();
        this.#trace?.socket(buffered);
        if (buffered > this.maxBufferedBytes) { await this.#sleep(20, 'socket'); continue; }
        const frame = this.#takeFrame(size);
        const now = this.now();
        const ms = size / BYTES_PER_MS;
        this.#sent += ms;
        if (this.#prebufLeft > 0) {
          // Still collecting on the device: everything sent so far is waiting there, nothing plays yet.
          this.#held += ms;
          this.#prebufLeft -= ms;
          this.#playEnd = now + this.#held;
        } else {
          this.#playEnd = Math.max(now, this.#playEnd) + ms;
        }
        this.sendAudio(frame);
        this.#trace?.sent(size, this.bufferedAmount());
      }
    } finally {
      this.#running = false;
      if (!this.#items.length) for (const resolve of this.#drainWaiters.splice(0)) resolve();
      else this.#pump();
    }
  }
}

/**
 * Pitch shift for s16le mono speech, duration preserved: WSOLA time-stretch by
 * the pitch ratio, then resample back. Formants move with the pitch, which is
 * exactly the small-cartoon-character timbre wanted for Kubik (`voice.pitch`).
 */
export function pitchShiftPcm16(pcm, semitones, sampleRate = SAMPLE_RATE) {
  const ratio = 2 ** (Number(semitones) / 12);
  const n = Math.floor(pcm.length / 2);
  if (!Number.isFinite(ratio) || Math.abs(semitones) < 0.05 || n < sampleRate / 20) return pcm.subarray(0, n * 2);
  const x = new Float32Array(n);
  for (let i = 0; i < n; i++) x[i] = pcm.readInt16LE(i * 2);
  const stretched = wsola(x, ratio, sampleRate);
  const outLen = Math.max(1, Math.round(stretched.length / ratio));
  const out = Buffer.alloc(outLen * 2);
  for (let i = 0; i < outLen; i++) {
    const p = i * ratio, k = Math.floor(p), f = p - k;
    const a = stretched[Math.min(k, stretched.length - 1)], b = stretched[Math.min(k + 1, stretched.length - 1)];
    out.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(a + (b - a) * f))), i * 2);
  }
  return out;
}

// Waveform-similarity overlap-add: output `alpha` times longer, pitch unchanged.
function wsola(x, alpha, sr) {
  const N = Math.round(0.03 * sr) & ~1, Hs = N / 2, Ha = Hs / alpha, tol = Math.round(0.008 * sr);
  const win = new Float32Array(N);
  for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / N);
  const frames = Math.max(1, Math.ceil((x.length - N) / Ha) + 1);
  const y = new Float32Array(frames * Hs + N);
  const at = (i) => (i >= 0 && i < x.length ? x[i] : 0);
  let prev = 0;  // input position of the previous frame
  for (let k = 0; k < frames; k++) {
    let pos = Math.round(k * Ha);
    if (k > 0) {
      // The frame that best continues the previous one's natural successor.
      const target = prev + Hs;
      let best = -Infinity, bestPos = pos;
      for (let d = -tol; d <= tol; d += 2) {
        const c = pos + d;
        if (c < 0 || c + N > x.length + N) continue;
        let s = 0;
        for (let i = 0; i < Hs; i += 2) s += at(c + i) * at(target + i);
        if (s > best) { best = s; bestPos = c; }
      }
      pos = bestPos;
    }
    for (let i = 0; i < N; i++) y[k * Hs + i] += at(pos + i) * win[i];
    prev = pos;
  }
  return y.subarray(0, Math.round(x.length * alpha));
}
