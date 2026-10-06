import { SpeechPacer } from './audio.js';
import { BYTES_PER_MS, MAX_NATIVE_AUDIO_BYTES, ImaEncoder, nextGen, out, pcmDurationMs, speechFrameIma } from './protocol.js';
import { chunkSpeech, parseReply, sanitizeForDisplay, splitShown } from './speech.js';
import { SessionControl } from './session-control.js';
import { SpeechStream } from './speech-stream.js';
import { runRecordedTurn } from './recorded-turn.js';
import { NativeSession } from './native-session.js';

const MAX_TURN_MS = 60_000;
const PLAYED_GRACE_MS = 2500;

/**
 * Per-connection state machine for one authenticated device.
 *
 * Cancellation model: `ptt on` (new utterance) and `cancel` bump `epoch`. Everything tied to an older
 * epoch — queued speech, the speech being generated, a pending transcription, and reply blocks that the
 * agent delivers later for the superseded turn — is dropped (and logged). The agent run itself is NOT
 * aborted: it finishes, its reply stays in the session history and any tool side effects happen.
 */
export class DeviceSession {
  #streams = [];
  #working = false;
  #wakeWorker = null;
  #speechAbort = null;
  #emotionTimers = new Set();
  #deliveryWait = null; // { kind, id, minPlayedMs, resolve, timer, promise }
  #receipt = 0;
  #textStream = null;
  #turn = null; // { id, epoch, phase: 'recording' | 'transcribing', startedAt, bytes, timer }
  #closed = false;
  #lastState = null;
  #interrupted = null; // { promise, resolve } replaced on every interrupt
  #activity = { own: '', other: '' };
  #activitySent = '|'; // a fresh connection starts with nothing to show (the device clears it on reconnect)
  #ima = null; // { gen, encoder } of the speech being sent as IMA ADPCM
  #native = null;
  #outputCancel = null;

  constructor({ ws, device, engine, dispatch, authorize = async () => true, log = () => {}, onActivity = () => {}, lastGen = 0,
    sessionId, volume = 0, textMode = 'auto', agentControl, current = () => this }) {
    Object.assign(this, { ws, device, engine, dispatch, authorize, log, onActivity, sessionId, volume, textMode, current });
    this.gen = lastGen;
    this.epoch = 0;
    this.pacer = new SpeechPacer({
      onTrace: (outcome, stats) => this.log(`kubik: ${this.device.id} speech ${outcome} gen=${this.activeGen} ${JSON.stringify(stats)}`),
      onStall: () => this.ws.close(1011, 'speech progress timeout'),
      sendAudio: (pcm) => {
        const gen = this.activeGen;
        if (gen == null) return;
        // One continuous ADPCM sequence per gen: the device decodes on from the state of the gen's first frame.
        if (this.#ima?.gen !== gen) this.#ima = { gen, encoder: new ImaEncoder() };
        this.#sendRaw(speechFrameIma(gen, this.#ima.encoder.encode(pcm)));
      },
      bufferedAmount: () => this.ws.bufferedAmount ?? 0,
    });
    this.activeGen = null;
    this.#armInterrupt();
    this.controls = new SessionControl(this, agentControl, () => Boolean(this.#turn || this.#streams.length || this.#deliveryWait));
  }

  #armInterrupt() {
    let resolve;
    const promise = new Promise((r) => { resolve = r; });
    this.#interrupted = { promise, resolve };
  }
  /** Resolves with the promise's value, or with undefined as soon as the session is interrupted. */
  #unlessInterrupted(promise) { return Promise.race([promise, this.#interrupted.promise]); }
  #clearTurn(turn) {
    if (this.#turn === turn) this.#turn = null;
    this.#kick();
  }

  get closed() { return this.#closed; }

  #sendRaw(data) {
    if (this.#closed || this.ws.readyState !== 1) return false;
    this.ws.send(data);
    return true;
  }
  send(message) { return this.#sendRaw(JSON.stringify(message)); }
  #setState(s) { this.#lastState = s; this.send(out.state(s)); }

  start() {
    this.send(out.welcome(this.sessionId, this.volume));
    this.sendCapabilities();
    this.#setState('idle');
    this.#sendActivity();
  }

  sendCapabilities() {
    this.send({ t: 'capabilities', voice_mode: this.engine.voiceMode ?? 'classic',
      stt: { available: this.engine.canListen !== false },
      tts: { available: this.engine.nativeVoice?.available ?? this.engine.canSpeak !== false } });
  }

  /** What OpenClaw is busy with (see out.activity); sent when it changes and again after a reconnect. */
  setActivity({ own = '', other = '' } = {}) {
    this.#activity = { own, other };
    this.#sendActivity();
  }
  #sendActivity() {
    const { own, other } = this.#activity;
    const key = `${own}|${other}`;
    if (key === this.#activitySent) return;
    if (this.send(out.activity(own, other))) this.#activitySent = key;
  }

  handleMessage(message) {
    switch (message.t) {
      case 'live_input_ack': this.#native?.acknowledge(message); return;
      case 'cancelled':
        if (this.#outputCancel?.gen === message.gen) this.#outputCancel.resolve();
        return;
      case 'ping': this.send(out.pong(message.ts)); return;
      case 'ptt': return message.on ? this.#pttOn(message.turn, message.automatic) : this.#pttOff(message.turn);
      case 'cancel':
        if (message.turn !== undefined) { this.#cancelTurn(message.turn); return; }
        this.interrupt('cancel'); this.#setState('idle'); return;
      case 'played':
        if (this.#deliveryWait?.kind === 'played' && this.#deliveryWait.id === message.gen &&
            Number.isInteger(message.ms) && message.ms >= this.#deliveryWait.minPlayedMs) this.#resolveAck(true);
        return;
      case 'shown':
        if (this.#textStream?.receipt === message.receipt) {
          this.#textStream.shownAck = message.receipt;
          if (this.#deliveryWait?.kind === 'shown' && this.#deliveryWait.id === message.receipt) this.#resolveAck(true);
        }
        return;
      case 'progress':
        if (message.gen === this.activeGen && Number.isFinite(message.ms)) this.pacer.played(message.ms);
        return;
      case 'poke': this.log(`kubik: ${this.device.id} poke ${message.kind}`); return;
      case 'device_state': this.volume = message.volume; return;
      case 'agent_options':
      case 'agent_model': return this.controls.handle(message);
      default: return;
    }
  }

  handleAudio({ turn, pcm, reference }) {
    if (this.#native?.turn.id === turn) { this.#native.worker?.append(pcm, reference); return; }
    const current = this.#turn;
    if (!current || current.phase !== 'recording' || current.id !== turn) return; // late or foreign frames
    if (pcmDurationMs(current.bytes + pcm.length) > MAX_TURN_MS) return;
    current.bytes += pcm.length;
    this.engine.append(pcm);
  }

  #cancelTurn(turnId) {
    if (this.#native?.turn.id === turnId) { this.interrupt('cancel'); this.#settleIdle(); return; }
    const turn = this.#turn;
    if (!turn || turn.id !== turnId) return;
    clearTimeout(turn.timer);
    turn.epoch = -1; // a pending commit may finish, but must never dispatch
    this.engine.cancelTranscription?.();
    this.#clearTurn(turn);
    this.#settleIdle();
  }

  #pttOn(turnId, automatic = false) {
    this.onActivity();
    if (this.controls.changingModel || (automatic &&
        (this.#turn || this.#streams.length || this.#deliveryWait || this.activeGen != null || this.#activity.own))) {
      this.send(out.error('busy'));
      return;
    }
    this.interrupt('ptt');
    if (this.engine.canListen === false) {
      this.send(out.text('Speech input is not available yet. Open Settings, then Agent, to check STT.', 'reply'));
      this.#setState('idle');
      return;
    }
    const turn = { id: turnId, epoch: this.epoch, phase: 'recording', startedAt: Date.now(), bytes: 0 };
    // A lost `ptt off` must not leave the turn open forever.
    if (this.engine.voiceMode !== 'live') {
      turn.timer = setTimeout(() => { if (this.#turn === turn && turn.phase === 'recording') this.#pttOff(turnId); }, MAX_TURN_MS + 5000);
      turn.timer.unref?.();
    }
    this.#turn = turn;
    if (this.engine.voiceMode && this.engine.voiceMode !== 'classic') { this.#beginNative(turn); return; }
    try { this.engine.beginTurn(); } catch (error) { this.log(`kubik: ${this.device.id} voice beginTurn failed: ${error.message}`); }
  }

  #pttOff(turnId) {
    if (this.#native?.turn.id === turnId) { this.#native.worker?.endInput(); return; }
    const turn = this.#turn;
    if (!turn || turn.phase !== 'recording' || turn.id !== turnId) return;
    clearTimeout(turn.timer);
    turn.phase = 'transcribing';
    this.#setState('transcribing');
    runRecordedTurn({ session: this, turn, clearTurn: (t) => this.#clearTurn(t), settleIdle: () => this.#settleIdle(),
      setState: (state) => this.#setState(state), openStream: (kind) => this.#openStream(kind),
      feed: (stream, text) => this.#feed(stream, text), endStream: (stream) => this.#endStream(stream) }).catch((error) => this.log(`kubik: ${this.device.id} turn failed: ${error?.message ?? error}`));
  }

  #beginNative(turn) {
    const current = () => !this.#closed && turn.epoch === this.epoch && !this.#native?.finished;
    try {
      this.#native = new NativeSession({ session: this, turn, dispatch: this.dispatch, current,
        openStream: kind => this.#openStream(kind), endStream: stream => this.#endStream(stream),
        clearTurn: value => this.#clearTurn(value), setState: state => this.#setState(state),
        show: (stream, text) => this.#show(stream, text) });
    } catch {
      this.#clearTurn(turn); this.send({ t: 'input_end', turn: turn.id, session: this.sessionId });
      this.send(out.error('voice_failed')); this.interrupt('native_start'); this.#setState('idle');
    }
  }
  wakeSpeech() { this.#kick(); }
  abortNative() { this.interrupt('native_end'); this.#setState('idle'); }

  #openStream(kind, beforeAttempt) {
    const stream = new SpeechStream(kind, this.epoch);
    stream.beforeAttempt = beforeAttempt;
    this.#streams.push(stream);
    this.#kick();
    return stream;
  }
  async #feed(stream, text) {
    const { speech, shown, inShow } = splitShown(text, { inShow: stream.inShow });
    stream.inShow = inShow;
    const { segments, pendingEmotion } = parseReply(speech, { carry: stream.carry });
    stream.carry = pendingEmotion;
    if (this.textMode === 'always' && segments.length) this.#show(stream, segments.map((s) => s.text).join(' '));
    for (const part of shown) this.#show(stream, part);
    for (const segment of segments) {
      let first = true;
      for (const chunk of chunkSpeech(segment.text, { first: !stream.chunked })) {
        if (!await stream.enqueue({ emotion: first ? segment.emotion : null, text: chunk })) return false;
        first = false;
        this.#kick();
      }
      stream.chunked = true;
    }
    this.#kick();
    return true;
  }

  /** Adds text to the stream's card on the device screen (the device replaces its card with the whole text). */
  #show(stream, text) {
    const add = sanitizeForDisplay(text);
    if (!add || stream.cancelled || stream.epoch !== this.epoch) return false;
    const joined = sanitizeForDisplay(stream.shown ? `${stream.shown}\n${add}` : add);
    if (joined === stream.shown) return true;
    if (!this.#beginNotifyAttempt(stream)) return false;
    stream.shown = joined;
    if (stream.timing && !stream.timing.audio && stream.textOnly) this.#logLatency(stream.timing, 'first text');
    stream.receipt = this.#receipt = (this.#receipt % 0xffffffff) + 1;
    this.#textStream = stream;
    return this.send(out.text(joined, stream.kind, stream.receipt));
  }
  #endStream(stream) { stream.ended = true; this.#kick(); }
  #kick() { this.#wakeWorker?.(); if (!this.#working) this.#work(); }

  /** After authorization, `beforeAttempt` marks the first output attempt.
   * Completion requires device playback or installation of the final text card. */
  async notify(text, { beforeAttempt = () => {} } = {}) {
    if (!await this.authorize() || this.#closed || this.current() !== this || this.ws.readyState !== 1) throw new Error('device disconnected');
    const stream = this.#openStream('notify', beforeAttempt);
    try { await this.#feed(stream, text); }
    finally { this.#endStream(stream); }
    return stream.done;
  }

  #beginNotifyAttempt(stream) {
    if (!stream.beforeAttempt) return true;
    try {
      if (this.#closed || this.current() !== this || this.ws.readyState !== 1) throw new Error('device disconnected');
      stream.beforeAttempt();
      stream.beforeAttempt = null;
      return true;
    } catch (error) {
      stream.failed = error;
      stream.items.length = 0;
      stream.ended = true;
      return false;
    }
  }

  async #work() {
    this.#working = true;
    let active;
    try {
      while (!this.#closed && this.#streams.length) {
        const stream = this.#streams[0];
        active = stream;
        if (stream.cancelled || stream.epoch !== this.epoch) {
          this.#streams.shift();
          stream.cancel();
          stream.resolve({ cancelled: true, spokenChars: stream.spokenChars,
            ...(stream.kind === 'notify' ? { status: 'interrupted', shownChars: stream.shown.length } : {}) });
          continue;
        }
        if (this.#turn && this.#turn.phase !== 'native' && stream.gen == null) { await this.#idleWait(); continue; } // never start talking over the user
        const segment = stream.take();
        if (segment) { await this.#speakSegment(stream, segment); continue; }
        if (!stream.ended) { await this.#idleWait(); continue; }
        this.#streams.shift();
        await this.#finishStream(stream);
      }
    } catch (error) {
      active?.cancel();
      active?.reject(error);
      this.close();
      this.ws.close(1011, 'speech worker failed');
      this.log(`kubik: ${this.device.id} speech worker failed: ${error?.message ?? error}`);
    } finally {
      this.#working = false;
    }
  }
  #idleWait() { return new Promise((resolve) => { this.#wakeWorker = () => { this.#wakeWorker = null; resolve(); }; }); }

  async #speakSegment(stream, segment) {
    if (stream.failed) return;
    if (stream.textOnly || this.volume < 20 || !Number.isFinite(this.volume) || (!segment.pcm && this.engine.canSpeak === false)) {
      // Nothing can be spoken: the reply is read from the screen.
      stream.textOnly = true;
      if (segment.emotion) this.send(out.emotion(segment.emotion));
      if (segment.text && this.textMode !== 'always') this.#show(stream, segment.text);
      return;
    }
    if (stream.gen == null) {
      await this.#waitDelivery(); // the previous gen must finish playing before a new one starts
      if (stream.cancelled || stream.epoch !== this.epoch || this.#closed) return;
      if (stream.beforeAttempt && !await this.authorize()) {
        stream.failed = new Error('device disconnected');
        return;
      }
      if (stream.cancelled || stream.epoch !== this.epoch || this.#closed) return;
      if (!this.#beginNotifyAttempt(stream)) return;
      this.gen = nextGen(this.gen);
      stream.gen = this.gen;
      this.activeGen = this.gen;
      if (segment.emotion) this.send(out.emotion(segment.emotion));
      this.send(out.speak(stream.gen, stream.kind));
      this.pacer.startGen(stream.trace);
      this.#setState('speaking');
    } else if (segment.emotion) {
      const emotion = segment.emotion;
      const epoch = this.epoch;
      this.pacer.mark((playAt) => this.#emotionAt(emotion, playAt, epoch));
    }
    if (segment.pcm) {
      this.pacer.push(segment.pcm); return;
    }
    const abort = new AbortController();
    this.#speechAbort = abort;
    const epoch = this.epoch;
    try {
      const result = await this.engine.speak(segment.text, {
        signal: abort.signal,
        onAudio: (pcm) => {
          if (abort.signal.aborted || epoch !== this.epoch) return;
          if (stream.timing && !stream.timing.audio) this.#logLatency(stream.timing);
          stream.trace.source(pcm.length); this.pacer.push(pcm);
        },
      });
      if (!result?.cancelled) stream.spokenChars += segment.text.length;
    } catch (error) {
      if (abort.signal.aborted) return;
      this.log(`kubik: ${this.device.id} speech failed: ${error?.message ?? error}`);
      if (epoch !== this.epoch) return;
      stream.textOnly = true;
      if (this.textMode !== 'always') this.#show(stream, segment.text);
      return;
    } finally {
      if (this.#speechAbort === abort) this.#speechAbort = null;
    }
  }

  #logLatency(t, what = 'first audio') {
    t.audio = Date.now();
    this.log(`kubik: ${this.device.id} turn ${t.turnId} latency: speech-to-text ${t.transcribed - t.released} ms, `
      + `agent ${(t.text ?? t.audio) - t.transcribed} ms, text-to-speech ${t.audio - (t.text ?? t.audio)} ms; `
      + `${what} ${t.audio - t.released} ms after the button`);
  }

  #emotionAt(emotion, playAt, epoch) {
    const delay = Math.max(0, playAt - performance.now());
    const timer = setTimeout(() => {
      this.#emotionTimers.delete(timer);
      if (epoch === this.epoch) this.send(out.emotion(emotion));
    }, delay);
    this.#emotionTimers.add(timer);
  }

  async #finishStream(stream) {
    let playback;
    if (stream.gen != null) {
      const gen = stream.gen;
      const epoch = this.epoch;
      await this.#unlessInterrupted(new Promise((resolve) => this.pacer.mark(resolve)));
      if (epoch === this.epoch && !this.#closed && !stream.cancelled) {
        this.send(out.speakEnd(gen));
        const played = this.#expectAck('played', gen, this.pacer.aheadOfRealtimeMs + PLAYED_GRACE_MS,
          Math.max(1, Math.floor(this.pacer.sentMs)));
        if (stream.live || (stream.kind === 'notify' && stream.spokenChars > 0)) playback = await played;
      }
    } else if (stream.kind === 'reply' && stream.epoch === this.epoch) {
      this.#settleIdle();
    }
    if (stream.kind === 'notify' && !stream.spokenChars && stream.receipt && !stream.cancelled &&
        stream.epoch === this.epoch && !this.#closed) {
      playback = stream.shownAck === stream.receipt ? { acknowledged: true } :
        await this.#expectAck('shown', stream.receipt, PLAYED_GRACE_MS);
    }
    const cancelled = stream.cancelled || stream.epoch !== this.epoch;
    if (stream.kind === 'notify' && this.#closed) stream.reject(new Error('device disconnected'));
    else if (stream.failed && stream.kind === 'notify') stream.reject(stream.failed);
    else if (stream.kind === 'notify' && !cancelled && !playback?.acknowledged) {
      stream.reject(new Error('device delivery acknowledgement timed out'));
    } else stream.resolve({ cancelled, spokenChars: stream.spokenChars, gen: stream.gen,
      ...(stream.live ? { played: playback?.acknowledged ?? stream.gen == null } : {}),
      ...(stream.kind === 'notify' ? { status: cancelled ? 'interrupted' : stream.spokenChars > 0 ? 'played' : 'shown',
        shownChars: stream.shown.length } : {}) });
  }

  #expectAck(kind, id, fallbackMs, minPlayedMs = 0) {
    this.#resolveAck();
    let resolve;
    const promise = new Promise((r) => { resolve = r; });
    const timer = setTimeout(() => this.#resolveAck(), fallbackMs);
    timer.unref?.();
    this.#deliveryWait = { kind, id, minPlayedMs, resolve, timer, promise };
    return promise;
  }
  #resolveAck(acknowledged = false) {
    const wait = this.#deliveryWait;
    if (!wait) return;
    this.#deliveryWait = null;
    clearTimeout(wait.timer);
    if (wait.kind === 'played' && this.activeGen === wait.id) {
      this.pacer.finishTrace(acknowledged ? 'played' : 'ack_timeout'); this.activeGen = null;
    }
    // The gen finished playing: an emotion scheduled for it is stale.
    for (const timer of this.#emotionTimers) clearTimeout(timer);
    this.#emotionTimers.clear();
    wait.resolve({ acknowledged });
    this.#settleIdle();
  }
  #waitDelivery() { return this.#outputCancel?.promise ?? this.#deliveryWait?.promise ?? Promise.resolve(); }

  /** Goes idle only when nothing else is happening. */
  #settleIdle() {
    if (this.#closed || this.#turn || this.#deliveryWait || this.#native?.live && !this.#native.finished) return;
    if (this.#streams.some((s) => !s.cancelled && s.epoch === this.epoch && (s.gen != null || s.items.length || !s.ended))) return;
    if (this.#lastState !== 'idle') this.#setState('idle');
  }

  /** Provider barge-in flushes only this Live reply, preserving capture, tools and session epoch. */
  async cancelLiveOutput(stream) {
    stream.cancel(); stream.resolve({ cancelled: true });
    if (this.#outputCancel) { this.#kick(); return this.#outputCancel.promise; }
    const gen = stream.gen;
    if (gen == null || gen !== this.activeGen) { this.#kick(); return; }
    this.pacer.finishTrace('barge_in'); this.pacer.clear(); this.#resolveAck(); this.activeGen = null;
    const interrupted = this.#interrupted; this.#armInterrupt(); interrupted.resolve();
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    const timer = setTimeout(() => reject(Error('Live output cancellation ACK timeout')), 3000);
    timer.unref?.();
    const wait = this.#outputCancel = { gen, promise, resolve, reject };
    this.send(out.speakCancel(gen)); this.#kick();
    try { await promise; }
    finally { clearTimeout(timer); if (this.#outputCancel === wait) this.#outputCancel = null; }
  }

  /** Stops speech, drops queued/pending work of the current epoch. The agent run is not aborted. */
  interrupt(reason) {
    this.epoch++;
    this.#outputCancel?.reject(Error("Live conversation closed"));
    this.#native?.close(); this.#native = null;
    const hadSpeech = this.activeGen != null || this.#streams.length > 0;
    this.#speechAbort?.abort();
    this.engine.cancel?.({ unplayedMs: this.pacer.aheadOfRealtimeMs + this.pacer.queuedBytes / BYTES_PER_MS });
    if (this.#turn) {
      clearTimeout(this.#turn.timer);
      this.engine.cancelTranscription?.();
      this.#turn = null;
    }
    this.pacer.finishTrace(reason); this.pacer.clear();
    for (const timer of this.#emotionTimers) clearTimeout(timer);
    this.#emotionTimers.clear();
    for (const stream of this.#streams) stream.cancel();
    if (this.#deliveryWait) { clearTimeout(this.#deliveryWait.timer); const wait = this.#deliveryWait; this.#deliveryWait = null; wait.resolve({ acknowledged: false }); }
    this.activeGen = null;
    const interrupted = this.#interrupted;
    this.#armInterrupt();
    interrupted.resolve();
    if (hadSpeech) this.log(`kubik: ${this.device.id} speech interrupted (${reason})`);
    this.#kick();
  }

  close() {
    if (this.#closed) return;
    this.#closed = true;
    for (const stream of this.#streams.splice(0)) { stream.cancel(); stream.reject(new Error('device disconnected')); }
    this.interrupt('disconnect');
    this.#wakeWorker?.();
  }
}
