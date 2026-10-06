import { buildRealtimeVoiceSessionInstructions, createRealtimeVoiceBridgeSession,
  isRealtimeVoiceAudioAudible, REALTIME_VOICE_AUDIO_FORMAT_PCM16_24KHZ } from 'openclaw/plugin-sdk/realtime-voice';
import { BYTES_PER_MS, MAX_NATIVE_AUDIO_BYTES } from './protocol.js';
import { delegateVoice, MAX_VOICE_TEXT } from './voice-delegate.js';
import { EchoCleaner } from './echo-cleaner.js';
import { isVoiceCommand } from './inbound.js';

/** One explicit live conversation, with many agent-backed replies on the same SDK connection. */
export class LiveVoiceSession {
  #bridge;
  #closed = false;
  #ready = false;
  #pending = [];
  #pendingBytes = 0;
  #consult = null;
  #echo = new EchoCleaner();
  #outputEpoch = 0;
  #settling = false;
  #tail = [];
  #tailBytes = 0;
  #heard = false;
  #lastAudio = 0;
  #quiet = null;
  #response = null;
  #deadline;
  #connecting;
  #userText = '';
  #text = '';
  #answer = '';
  #shown = '';

  constructor({ config, port, dispatch, turnId, createBridge = createRealtimeVoiceBridgeSession,
    connectMs = 10_000, sessionMs = 30 * 60_000, responseMs = 120_000, quietMs = 3000 }) {
    Object.assign(this, { config, port, dispatch, turnId, responseMs, quietMs });
    this.#bridge = createBridge({ ...config, audioFormat: REALTIME_VOICE_AUDIO_FORMAT_PCM16_24KHZ,
      autoRespondToAudio: true, interruptResponseOnInputAudio: true,
      instructions: buildRealtimeVoiceSessionInstructions({ base:
        `You are Tess, the voice of the connected agent.${config.language ? ` Speak in ${config.language}.` : ''}`,
        isAgentProxy: true, toolPolicy: 'owner', consultPolicy: 'always' }),
      runAgentConsult: request => this.#runAgent(request),
      audioSink: { isOpen: () => this.#current(), sendAudio: pcm => this.#audio(pcm),
        clearAudio: () => this.#clearAudio() },
      onTranscript: (role, text, final, metadata) => this.#transcript(role, text, final, metadata),
      onResponseDone: outcome => {
        if (['failed', 'incomplete'].includes(outcome.status)) this.fail();
        else if (outcome.status === 'completed' && !this.#consult) this.#finishReply();
      },
      onError: () => this.fail(), onClose: () => { if (!this.#closed) this.fail(); }, markStrategy: 'ignore' });
    this.#deadline = setTimeout(() => this.fail('session_limit'), sessionMs);
    this.#connecting = setTimeout(() => this.fail('connect_timeout'), connectMs);
    this.#deadline.unref?.(); this.#connecting.unref?.();
    Promise.resolve().then(() => this.#bridge.connect()).then(async () => {
      clearTimeout(this.#connecting);
      if (!this.#current()) return;
      await port.resumeInput();
      if (!this.#current()) return;
      this.#ready = true;
      for (const pcm of this.#pending.splice(0)) if (!this.#send(pcm)) return;
      this.#pendingBytes = 0;
    }).catch(() => this.fail());
  }
  #current() { return !this.#closed && this.port.isCurrent(); }
  append(pcm, reference) {
    if (!this.#current()) return;
    try { pcm = this.#echo.process(pcm, reference); }
    catch { this.fail("echo_input_invalid"); return; }
    if (this.#ready) { this.#send(pcm); return; }
    this.#pendingBytes += pcm.length;
    if (this.#pendingBytes > 5000 * BYTES_PER_MS) { this.fail('input_queue_full'); return; }
    this.#pending.push(Buffer.from(pcm));
  }
  #send(pcm) {
    try { this.#bridge.sendAudio(pcm); return true; }
    catch { this.fail(); return false; }
  }
  #transcript(role, text, final, metadata) {
    if (!this.#current() || typeof text !== 'string') return;
    const previous = role === 'user' ? this.#userText : this.#text;
    const next = metadata?.textMode === 'snapshot' || final ? text : previous + text;
    if (next.length > MAX_VOICE_TEXT) { this.fail('text_limit'); return; }
    if (role === 'user') this.#userText = next;
    else { this.#text = next; if (!this.#shown) this.port.text(next); }
  }
  async #runAgent({ prompt, signal }) {
    if (!this.#current() || typeof prompt !== 'string' || !prompt.trim() || prompt.length > MAX_VOICE_TEXT)
      throw Error('Invalid live delegation');
    this.#startResponse();
    const transcript = isVoiceCommand(this.#userText) ? this.#userText : prompt;
    this.#userText = '';
    const current = () => this.#current() && !signal?.aborted;
    const consult = delegateVoice({ dispatch: this.dispatch, transcript, turnId: this.turnId,
      port: this.port, isCurrent: current });
    this.#consult = consult;
    let screenOnly = false;
    try {
      const result = await consult;
      if (!current()) throw signal?.reason ?? Error('Live delegation superseded');
      this.#answer = result.answer; this.#shown = result.shown;
      this.#heard = false; clearTimeout(this.#quiet);
      screenOnly = !result.text;
      return result;
    } catch (error) {
      if (!signal?.aborted) this.fail();
      throw error;
    } finally {
      if (this.#consult === consult) {
        this.#consult = null;
        if (screenOnly) this.#finishReply(); else this.#checkQuiet();
      }
    }
  }
  #startResponse() {
    clearTimeout(this.#response);
    this.#response = setTimeout(() => this.fail('response_timeout'), this.responseMs);
    this.#response.unref?.();
  }
  #clearAudio() {
    if (!this.#current()) return;
    const epoch = ++this.#outputEpoch;
    clearTimeout(this.#quiet); clearTimeout(this.#response);
    this.#response = null;
    this.#tail = []; this.#tailBytes = 0;
    this.#settling = true; this.#heard = false;
    this.#text = this.#answer = this.#shown = '';
    Promise.resolve(this.port.cancelReply()).then(() => {
      if (this.#current() && epoch === this.#outputEpoch) this.#drainTail();
    }).catch(() => { if (this.#current()) this.fail('output_cancel_failed'); });
  }
  #audio(pcm) {
    if (!this.#current() || !pcm.length) return;
    if (pcm.length % 2 || pcm.length > MAX_NATIVE_AUDIO_BYTES) { this.fail('invalid_audio'); return; }
    const audible = isRealtimeVoiceAudioAudible(pcm, REALTIME_VOICE_AUDIO_FORMAT_PCM16_24KHZ);
    if (audible && !this.#response) this.#startResponse();
    if (this.#settling) {
      // Playback receipts close an output segment only. Capture keeps streaming;
      // retain late output while the preceding segment drains.
      if (!audible && !this.#tail.length) return;
      this.#tailBytes += pcm.length;
      if (this.#tailBytes > 5000 * BYTES_PER_MS) { this.fail('audio_queue_full'); return; }
      this.#tail.push(Buffer.from(pcm)); return;
    }
    if (!audible && !this.#heard) return;
    if (audible) { this.#lastAudio = Date.now(); this.#heard = true; }
    if (!this.port.audio(pcm)) { this.fail('audio_queue_full'); return; }
    this.#checkQuiet();
  }
  #checkQuiet() {
    if (!this.#current() || !this.#heard || this.#consult || this.#settling) return;
    clearTimeout(this.#quiet);
    this.#quiet = setTimeout(() => this.#finishReply(), Math.max(0, this.quietMs - (Date.now() - this.#lastAudio)));
    this.#quiet.unref?.();
  }
  async #finishReply() {
    if (!this.#current() || this.#consult || this.#settling) return;
    const epoch = this.#outputEpoch;
    this.#settling = true; clearTimeout(this.#quiet);
    try {
      await this.port.finishReply(); // receipt tracks output; it never closes capture
      if (!this.#current() || epoch !== this.#outputEpoch) return;
      if (this.#tail.length) { this.#drainTail(); return; }
      this.#settling = this.#heard = false;
      this.#text = this.#answer = this.#shown = '';
      clearTimeout(this.#response);
      this.#response = null;
      this.#drainTail();
    } catch { if (epoch === this.#outputEpoch) this.fail('playback_failed'); }
  }
  #drainTail() {
    this.#settling = this.#heard = false;
    const tail = this.#tail.splice(0); this.#tailBytes = 0;
    for (const pcm of tail) this.#audio(pcm);
  }
  endInput() { if (this.#current()) { this.close(); this.port.finish(); } }
  fail(reason = 'provider') {
    if (this.#closed) return;
    if (this.#answer) this.port.text(this.#answer, true);
    this.close(); this.port.fail(reason);
  }
  close() {
    if (this.#closed) return;
    this.#closed = true;
    clearTimeout(this.#deadline); clearTimeout(this.#connecting); clearTimeout(this.#quiet); clearTimeout(this.#response);
    this.#echo.close();
    this.#pending = []; this.#pendingBytes = 0;
    this.#tail = []; this.#tailBytes = 0;
    Promise.resolve().then(() => this.#bridge.close({ disposition: 'detach' })).catch(() => {});
  }
}
