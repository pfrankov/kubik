import { BYTES_PER_MS } from './protocol.js';

/** Constant-size timing counters; never retain PCM, transcripts or per-frame logs. */
export class SpeechTrace {
  #now;
  #started = null;
  #last = {};
  #stats = { sourceChunks: 0, sourceMs: 0, sourceMaxMs: 0, sourceGapMs: 0,
    sentFrames: 0, sentMs: 0, sendGapMs: 0, progressReports: 0, progressGapMs: 0,
    confirmedMs: 0, maxInFlightMs: 0, socketMaxBytes: 0,
    creditWaitMs: 0, batchWaitMs: 0, socketWaitMs: 0 };

  constructor(now = () => performance.now()) { this.#now = now; }

  #gap(kind, field) {
    const now = this.#now();
    if (this.#last[kind] !== undefined) this.#stats[field] = Math.max(this.#stats[field], now - this.#last[kind]);
    this.#last[kind] = now;
  }

  source(bytes) {
    this.#started ??= this.#now();
    this.#gap('source', 'sourceGapMs');
    this.#stats.sourceChunks++;
    this.#stats.sourceMs += bytes / BYTES_PER_MS;
    this.#stats.sourceMaxMs = Math.max(this.#stats.sourceMaxMs, bytes / BYTES_PER_MS);
  }

  sent(bytes, buffered) {
    this.#gap('send', 'sendGapMs');
    this.#stats.sentFrames++;
    this.#stats.sentMs += bytes / BYTES_PER_MS;
    this.#stats.maxInFlightMs = Math.max(this.#stats.maxInFlightMs, this.#stats.sentMs - this.#stats.confirmedMs);
    this.socket(buffered);
  }

  progress(ms) {
    this.#gap('progress', 'progressGapMs');
    this.#stats.progressReports++;
    this.#stats.confirmedMs = ms;
  }

  socket(bytes) { this.#stats.socketMaxBytes = Math.max(this.#stats.socketMaxBytes, bytes); }
  waited(kind, ms) { this.#stats[`${kind}WaitMs`] += ms; }

  summary() {
    const now = this.#now();
    return Object.fromEntries(Object.entries({ ...this.#stats, elapsedMs: now - (this.#started ?? now),
      sourceAgeMs: now - (this.#last.source ?? now), sendAgeMs: now - (this.#last.send ?? now),
      progressAgeMs: now - (this.#last.progress ?? this.#started ?? now) }).map(([key, value]) => [key, Math.round(value)]));
  }
}
