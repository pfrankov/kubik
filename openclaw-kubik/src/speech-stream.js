import { SpeechTrace } from './speech-trace.js';
const MAX_QUEUED_SEGMENTS = 32;

/** One spoken reply or notification. Producers wait for playback to free queue space. */
export class SpeechStream {
  #room = null;
  #releaseRoom = null;

  constructor(kind, epoch) {
    this.kind = kind; this.epoch = epoch; this.items = []; this.ended = false; this.cancelled = false;
    this.trace = new SpeechTrace();
    this.carry = null; this.gen = null; this.spokenChars = 0; this.failed = null;
    // inShow tracks an unclosed [[show]] block; shown is the current device card.
    this.inShow = false; this.chunked = false; this.shown = ''; this.textOnly = false;
    this.done = new Promise((resolve, reject) => { this.resolve = resolve; this.reject = reject; });
    this.done.catch(() => {});
  }

  async enqueue(segment) {
    while (!this.cancelled && this.items.length >= MAX_QUEUED_SEGMENTS) {
      this.#room ??= new Promise((resolve) => { this.#releaseRoom = resolve; });
      await this.#room;
    }
    if (this.cancelled) return false;
    this.items.push(segment);
    return true;
  }

  take() {
    const segment = this.items.shift();
    this.#wakeProducer();
    return segment;
  }

  /** Native providers may emit arbitrarily small PCM deltas in one burst. */
  enqueuePcm(pcm, frameBytes) {
    let at = 0;
    const tail = this.items.at(-1);
    if (tail?.pcm && tail.pcm.length < frameBytes) {
      at = Math.min(pcm.length, frameBytes - tail.pcm.length);
      tail.pcm = Buffer.concat([tail.pcm, pcm.subarray(0, at)]);
    }
    for (; at < pcm.length; at += frameBytes)
      this.items.push({ pcm: Buffer.from(pcm.subarray(at, at + frameBytes)) });
  }

  cancel() {
    this.cancelled = true;
    this.items.length = 0;
    this.#wakeProducer();
  }

  #wakeProducer() {
    this.#releaseRoom?.();
    this.#room = this.#releaseRoom = null;
  }
}
