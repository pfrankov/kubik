import { nextGen, out } from './protocol.js';

/** One printer reservation and the session's sole speech worker share output ownership. */
export class SessionOutputLease {
  #lease = null;
  #speechWait = null;
  #starting = false;
  #closed = false;
  #changedQueued = false;
  #wakeSpeech = false;
  #uncertain = null;

  constructor({ idle }) { this.idle = idle; }

  acquire(signal) {
    if (this.#closed || signal.aborted) return Promise.reject(Error('Output lease cancelled'));
    if (this.#lease) return Promise.reject(Error('Output lease busy'));
    const lease = { signal, waiting: true, resolve: null, reject: null, abort: null };
    const promise = new Promise((resolve, reject) => { Object.assign(lease, { resolve, reject }); });
    this.#lease = lease;
    lease.abort = () => {
      if (lease.waiting) lease.reject(Error('Output lease cancelled'));
      this.#release(lease);
    };
    signal.addEventListener('abort', lease.abort, { once: true });
    this.changed();
    return promise;
  }

  #release(lease) {
    if (this.#lease !== lease) return;
    this.#lease = null;
    lease.signal.removeEventListener('abort', lease.abort);
    this.#speechWait?.resolve();
    this.#speechWait = null;
  }

  changed(wakeSpeech = false) {
    this.#wakeSpeech ||= wakeSpeech;
    if (this.#changedQueued) return;
    this.#changedQueued = true;
    // Cancellation can clear activeGen before installing its ACK barrier in the same turn.
    queueMicrotask(() => {
      this.#changedQueued = false;
      if (this.#wakeSpeech) { this.#wakeSpeech = false; this.#speechWait?.resolve(); this.#speechWait = null; }
      const lease = this.#lease;
      if (!lease?.waiting || this.#starting || this.#uncertain || !this.idle()) return;
      lease.waiting = false;
      lease.resolve(() => this.#release(lease));
    });
  }

  uncertain(gen, minimum = 0) { this.#uncertain = { gen, minimum }; }

  acknowledge(message) {
    const pending = this.#uncertain;
    if (!pending) return;
    if (message.t === 'ptt' && message.on && message.automatic !== true) {
      this.#uncertain = null; this.changed(); return;
    }
    if (message.gen !== pending.gen) return;
    const played = message.t === 'played' && Number.isInteger(message.ms) && message.ms >= pending.minimum;
    if (message.t !== 'cancelled' && !played) return;
    this.#uncertain = null;
    this.changed();
  }

  async speech(begin, valid, authorize) {
    while (!this.#closed && valid()) {
      if (!this.#lease) {
        if (!await authorize() || this.#closed || !valid()) return false;
        if (this.#lease) continue;
        this.#starting = true;
        try {
          const started = begin();
          if (started) this.#uncertain = null;
          return started;
        }
        finally { this.#starting = false; this.changed(); }
      }
      this.#speechWait ??= {};
      this.#speechWait.promise ??= new Promise(resolve => { this.#speechWait.resolve = resolve; });
      await this.#speechWait.promise;
    }
    return false;
  }

  close() {
    this.#closed = true;
    this.#lease?.abort();
    this.#speechWait?.resolve();
    this.#speechWait = null;
  }
}

const current = (session, stream) => !stream.cancelled && stream.epoch === session.epoch && !session.closed;

/** Starts a new generation atomically with the printer reservation after delivery and authorization waits. */
export async function beginSpeechGeneration(session, stream, segment, { delivery, attempt, state }) {
  await delivery();
  if (!current(session, stream)) return false;
  return session.withOutputGeneration(() => {
    if (!current(session, stream) || !attempt(stream)) return false;
    session.gen = nextGen(session.gen);
    stream.gen = session.activeGen = session.gen;
    if (segment.emotion) session.send(out.emotion(segment.emotion));
    session.send(out.speak(stream.gen, stream.kind));
    stream.voiceTrace?.phase('host-output');
    session.pacer.startGen(stream.trace);
    state('speaking');
    return true;
  }, () => current(session, stream), async () => {
    if (!stream.beforeAttempt || await session.authorize()) return true;
    stream.failed = Error('device disconnected'); return false;
  });
}
