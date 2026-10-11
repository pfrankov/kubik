// A bounded phase trace, no transcripts, PCM, credentials or per-frame logs.
export class VoiceTurnTrace {
  #seen = new Set();
  constructor(log, deviceId, turnId, now = () => performance.now()) {
    Object.assign(this, { log, deviceId, turnId, now });
    this.started = now();
  }
  phase(name) {
    if (this.#seen.has(name) || this.#seen.size >= 24) return;
    this.#seen.add(name);
    this.log(`kubik: ${this.deviceId} turn ${this.turnId} phase ${name} +${Math.round(this.now() - this.started)}ms`);
  }
}

export function logRecordedLatency(log, deviceId, t, what = 'first audio') {
  t.audio = Date.now();
  log(`kubik: ${deviceId} turn ${t.turnId} latency: speech-to-text ${t.transcribed - t.released} ms, `
    + `agent ${(t.text ?? t.audio) - t.transcribed} ms, text-to-speech ${t.audio - (t.text ?? t.audio)} ms; `
    + `${what} ${t.audio - t.released} ms after the button`);
}
