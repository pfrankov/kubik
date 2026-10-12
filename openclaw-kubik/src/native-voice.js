import {
  buildRealtimeVoiceSessionInstructions,
  createRealtimeVoiceBridgeSession, getRealtimeVoiceProvider,
  REALTIME_VOICE_AUDIO_FORMAT_PCM16_24KHZ, resolveRealtimeVoiceProviderCapabilities,
} from 'openclaw/plugin-sdk/realtime-voice';
import { BYTES_PER_MS, MAX_NATIVE_AUDIO_BYTES } from './protocol.js';
import { delegateVoice, MAX_VOICE_TEXT as MAX_TEXT } from './voice-delegate.js';
import { RealtimeReply } from './realtime-reply.js';

const MODELS = { realtime: 'gpt-realtime-2.1', live: 'gpt-live-1' };
const MAX_INPUT_BYTES = 60_000 * BYTES_PER_MS;
// GPT-Live's public PCM bridge also bounds its pending input to 240 KB.
const MAX_PENDING_BYTES = 5000 * BYTES_PER_MS;

/** Metadata/auth readiness only: no provider connection, audio or inference. */
export async function resolveNativeVoice({ cfg, voice, agentId, sdk = {} }) {
  if (!MODELS[voice?.mode]) return null;
  const provider = (sdk.getRealtimeVoiceProvider ?? getRealtimeVoiceProvider)('openai', cfg);
  if (!provider) return { mode: voice.mode, available: false, stt: { available: false }, tts: { available: false } };
  const providerConfig = provider.resolveConfig({ cfg, agentId, surface: 'gateway-relay',
    // Match the device's pause; the SDK's 500 ms default cuts ordinary short hesitations.
    rawConfig: { model: voice.model ?? MODELS[voice.mode], voice: voice.voice, silenceDurationMs: 800 }, autoRespondToAudio: voice.mode === 'live' });
  const capabilities = (sdk.resolveRealtimeVoiceProviderCapabilities ?? resolveRealtimeVoiceProviderCapabilities)({
    provider, cfg, agentId, providerConfig, surface: 'gateway-relay' });
  const supported = !voice.model || (provider.models?.includes(voice.model) &&
    (voice.mode === 'live') === voice.model.startsWith('gpt-live-'));
  const available = supported && await provider.isConfigured({ cfg, agentId, providerConfig });
  const metadata = { available, ...(available ? { provider: 'openai', model: providerConfig.model } : {}) };
  return { cfg, agentId, provider, providerConfig, capabilities, mode: voice.mode,
    language: voice.language, available, stt: metadata, tts: metadata };
}

/** One KEY-owned native connection. It delegates through the authenticated device's existing dispatcher. */
export class NativeVoiceTurn {
  #bridge;
  #closed = false;
  #ready = false;
  #input = true;
  #pending = [];
  #pendingBytes = 0;
  #inputBytes = 0;
  #tail = null;
  #deadline = null;
  #connecting = null;
  #consult = null;
  #consulted = false;
  #answer = '';
  #reply;

  constructor({ config, port, dispatch, turnId, createBridge = createRealtimeVoiceBridgeSession,
    connectMs = 10_000, turnMs = 120_000 }) {
    Object.assign(this, { config, port, dispatch, turnId });
    this.#reply = new RealtimeReply({ send: text => this.#bridge.sendUserMessage(text),
      emotion: emotion => port.emotion?.(emotion), finish: () => this.finish(), fail: () => this.fail(),
      phase: name => port.phase?.(name) });
    this.#bridge = createBridge({ ...config,
      audioFormat: REALTIME_VOICE_AUDIO_FORMAT_PCM16_24KHZ,
      autoRespondToAudio: false,
      // Input is gated; acoustic interruption is enabled only once device AEC is verified.
      interruptResponseOnInputAudio: false,
      instructions: buildRealtimeVoiceSessionInstructions({ base:
        `You are Tess, the voice of the connected agent.${config.language ? ` Speak in ${config.language}.` : ''}`,
        isAgentProxy: true, toolPolicy: 'owner', consultPolicy: 'always' }),
      audioSink: { isOpen: () => !this.#closed && port.isCurrent(), sendAudio: (pcm) => this.#audio(pcm) },
      onTranscript: (role, text, final) => this.#transcript(role, text, final),
      onEvent: ({ type }) => { if (type === 'input_audio_buffer.speech_stopped') this.endInput(); },
      onResponseDone: (outcome) => {
        if (outcome.status === 'failed' || outcome.status === 'incomplete') this.fail();
        else if (outcome.status === 'completed') this.#reply.completed();
      },
      onError: () => this.fail(),
      onClose: () => { if (!this.#closed) this.fail(); },
      markStrategy: 'ignore',
    });
    this.#deadline = setTimeout(() => this.fail(), turnMs);
    this.#deadline.unref?.();
    this.#connecting = setTimeout(() => this.fail(), connectMs);
    this.#connecting.unref?.();
    Promise.resolve().then(() => this.#closed ? undefined : this.#bridge.connect()).then(() => {
      clearTimeout(this.#connecting);
      if (this.#closed) return;
      this.#ready = true;
      port.phase?.('provider-ready');
      for (const pcm of this.#pending.splice(0)) if (!this.#sendInput(pcm)) return;
      this.#pendingBytes = 0;
      if (!this.#input) this.#sendTail();
    }).catch(() => { clearTimeout(this.#connecting); this.fail(); });
  }

  append(pcm) {
    if (this.#closed || !this.#input || !this.port.isCurrent()) return;
    this.#inputBytes += pcm.length;
    if (this.#inputBytes > MAX_INPUT_BYTES) { this.endInput(); return; }
    if (this.#ready) { this.#sendInput(pcm); return; }
    this.#pendingBytes += pcm.length;
    if (this.#pendingBytes > MAX_PENDING_BYTES) { this.fail(); return; }
    this.#pending.push(Buffer.from(pcm));
  }

  endInput() {
    if (this.#closed || !this.#input) return;
    this.#input = false;
    this.port.phase?.('input-ended');
    this.port.endInput();
    if (this.#ready) this.#sendTail();
  }

  #sendInput(pcm) {
    try { this.#bridge.sendAudio(pcm); return true; }
    catch { this.fail(); return false; }
  }

  #sendTail() {
    if (this.#tail || this.#bridge.bridge.pacesInputAudio) return;
    // The public SDK has no commit operation. A bounded silence tail triggers provider VAD.
    let remaining = 1000;
    this.#tail = setInterval(() => {
      if (this.#closed || remaining <= 0) { clearInterval(this.#tail); this.#tail = null; return; }
      if (!this.#sendInput(Buffer.alloc(40 * BYTES_PER_MS))) return;
      remaining -= 40;
    }, 40);
    this.#tail.unref?.();
  }

  #transcript(role, text, final) {
    if (this.#closed || !this.port.isCurrent() || typeof text !== 'string') return;
    if (role === 'user') {
      if (text.length > MAX_TEXT) { this.fail(); return; }
      if (!final) return;
      this.port.phase?.('transcript');
      this.endInput();
      if (!this.#consulted) {
        this.#runAgent(text).catch(() => this.fail());
      }
      return;
    }
    // The agent owns screen text. A synthesis transcript describes only the
    // current spoken block and must not replace the complete answer/card.
  }

  async #runAgent(prompt) {
    if (this.#closed || !this.port.isCurrent()) throw new Error('Voice turn is closed');
    if (this.#consult) return this.#consult;
    if (this.#consulted || typeof prompt !== 'string' || !prompt.trim() || prompt.length > MAX_TEXT) throw new Error('Invalid voice delegation');
    this.endInput();
    this.#consulted = true;
    this.#consult = delegateVoice({ dispatch: this.dispatch, transcript: prompt, turnId: this.turnId,
      port: this.port, isCurrent: () => !this.#closed && this.port.isCurrent(),
      onBlock: answer => { this.#answer = answer; },
      onSpeech: segments => this.#reply.enqueue(segments), onPhase: name => this.port.phase?.(name) }).then(result => {
      this.#answer = result.answer; return result;
    }).finally(() => { this.#consult = null; }).then(result => { this.#reply.end(); return result; });
    return this.#consult;
  }

  #audio(pcm) {
    if (this.#closed || this.#input || !this.port.isCurrent() || !pcm.length) return;
    if (pcm.length % 2 || pcm.length > MAX_NATIVE_AUDIO_BYTES) { this.fail('invalid_audio'); return; }
    if (!this.port.audio(pcm)) this.fail('audio_queue_full');
  }

  finish() { if (!this.#closed) { this.endInput(); this.close(); this.port.finish(); } }
  fail(reason = 'provider') {
    if (this.#closed) return;
    this.endInput();
    if (this.#answer) this.port.text(this.#answer, true);
    this.close(); this.port.fail(reason);
  }
  close() {
    if (this.#closed) return;
    this.#closed = true;
    clearInterval(this.#tail); clearTimeout(this.#deadline); clearTimeout(this.#connecting);
    this.#reply.close();
    this.#pending = []; this.#pendingBytes = 0;
    // Cancel voice, not accepted agent/tool work. That work remains in the agent's existing session.
    Promise.resolve().then(() => this.#bridge.close({ disposition: 'detach' })).catch(() => {});
  }
}
