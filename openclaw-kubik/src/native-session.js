import { BYTES_PER_MS, MAX_NATIVE_AUDIO_BYTES, out } from './protocol.js';

/** Session-owned input ACKs and independent playback streams for a native conversation. */
export class NativeSession {
  #inputAck = null;
  #cancelBarrier = null;
  #inputTail = Promise.resolve();
  constructor({ session, turn, dispatch, openStream, endStream, clearTurn, setState, show, current }) {
    Object.assign(this, { session, turn, openStream, endStream, clearTurn, setState, show, current });
    this.live = session.engine.voiceMode === 'live';
    this.stream = this.#stream();
    const muted = () => !Number.isFinite(session.volume) || session.volume < 20;
    this.worker = session.engine.createVoiceTurn({ turnId: turn.id, dispatch,
      port: { isCurrent: current, endInput: () => this.#endInput(), finish: () => this.#finish(),
        pauseInput: () => this.#input(false), resumeInput: () => this.#input(true),
        finishReply: () => this.#finishReply(), cancelReply: () => this.#cancelReply(),
        fail: reason => this.#fail(reason),
        emotion: emotion => { if (current()) session.send(out.emotion(emotion)); },
        reply: (answer, shown) => {
          const text = muted() || session.textMode === 'always' ? answer : shown;
          if (current() && text) this.#text(text);
        },
        text: (text, force = false) => {
          if (current() && (force || muted() || session.textMode === 'always')) this.#text(text);
        },
        audio: pcm => {
          if (!current()) return false;
          if (muted()) return true;
          const stream = this.stream ??= this.#stream();
          const queued = session.pacer.queuedBytes + stream.items.reduce((bytes, item) => bytes + (item.pcm?.length ?? 0), 0);
          if (queued + pcm.length > MAX_NATIVE_AUDIO_BYTES) return false;
          stream.trace.source(pcm.length); stream.enqueuePcm(pcm, 400 * BYTES_PER_MS);
          session.wakeSpeech(); return true;
        } } });
  }
  #stream() { const stream = this.openStream('reply'); stream.live = this.live; return stream; }
  #text(text) { const stream = this.stream ??= this.#stream(); stream.shown = ''; this.show(stream, text); }
  #endInput() {
    if (this.ending || !this.current()) return this.ending;
    clearTimeout(this.turn.timer);
    this.ending = this.#input(false).then(() => {
      if (!this.current()) return;
      this.session.send({ t: 'input_end', turn: this.turn.id, session: this.session.sessionId });
      this.clearTurn(this.turn); this.setState('thinking');
    }).catch(() => this.#fail('input_pause_failed'));
    return this.ending;
  }
  #fail(reason = 'provider') {
    this.session.log(`kubik: ${this.session.device.id} native voice failed (${reason})`);
    if (!this.current()) return;
    this.session.send({ t: 'input_end', turn: this.turn.id, session: this.session.sessionId });
    this.session.send(out.error('voice_failed')); this.session.abortNative();
  }
  #input(on) {
    const next = this.#inputTail.then(() => this.#requestInput(on));
    this.#inputTail = next.catch(() => {});
    return next;
  }
  #requestInput(on) {
    if (!this.current()) return Promise.reject(Error('Native conversation is closed'));
    if (!on) this.turn.phase = 'pausing';
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.#inputAck = null; reject(Error('Live microphone ACK timeout')); }, 3000);
      timer.unref?.();
      this.#inputAck = { on, resolve, reject, timer };
      this.session.send({ t: 'live_input', turn: this.turn.id, on });
    });
  }
  acknowledge(message) {
    const wait = this.#inputAck;
    if (!wait || message.turn !== this.turn.id || message.on !== wait.on || !this.current()) return;
    clearTimeout(wait.timer); this.#inputAck = null;
    this.turn.phase = message.on && !this.live ? 'recording' : 'native';
    this.setState(message.on ? 'listening' : 'thinking');
    wait.resolve(); this.session.wakeSpeech();
  }
  async #finishReply() {
    const stream = this.stream;
    if (!this.current() || !stream) return;
    this.endStream(stream);
    const result = await stream.done;
    if (!this.current() || result.cancelled || (stream.gen != null && !result.played))
      throw Error('Live reply was not played');
    if (this.stream === stream) { this.stream = null; this.setState("listening"); }
  }
  async #cancelReply() {
    const stream = this.stream; this.stream = null;
    if (stream) {
      const cancel = this.session.cancelLiveOutput(stream);
      this.#cancelBarrier = cancel;
      try { await cancel; }
      finally { if (this.#cancelBarrier === cancel) this.#cancelBarrier = null; }
    } else await this.#cancelBarrier;
    if (this.current()) this.setState("listening");
  }
  async #finish() {
    if (!this.current()) return;
    if (this.live) {
      this.session.send({ t: 'input_end', turn: this.turn.id, session: this.session.sessionId });
      this.session.abortNative(); return;
    }
    await this.ending;
    if (!this.current()) return;
    clearTimeout(this.turn.timer); this.clearTurn(this.turn);
    if (this.stream) this.endStream(this.stream);
    this.finished = true; this.setState('idle');
  }
  close() {
    if (this.#inputAck) {
      clearTimeout(this.#inputAck.timer); this.#inputAck.reject(Error('Native conversation is closed')); this.#inputAck = null;
    }
    this.worker?.close();
  }
}
