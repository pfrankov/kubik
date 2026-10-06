import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { delimiter, join } from 'node:path';
import { isRecord } from '../config.js';
import { mulawToPcm16, parseWav, pcmToWav, resamplePcm16 } from '../audio.js';
import { BYTES_PER_MS, SAMPLE_RATE } from '../protocol.js';
import { VoiceError } from './common.js';
import { LiveTranscription, openAiCredential, resolveLiveCredential } from '../live-transcription.js';
import { streamDeviceSpeech } from '../stream-speech.js';
import { getResolvedSpeechProviderConfig, getTtsProvider, isTtsProviderConfigured, resolveTtsConfig, resolveTtsPrefsPath } from 'openclaw/plugin-sdk/tts-runtime';
import { getSpeechProvider } from 'openclaw/plugin-sdk/speech';
import { access, readdir } from 'node:fs/promises';
import { constants } from 'node:fs';
import { NativeVoiceTurn, resolveNativeVoice } from '../native-voice.js';
import { LiveVoiceSession } from '../live-voice.js';

const MAX_TURN_BYTES = 60_000 * BYTES_PER_MS; // one minute of speech
const FAILED_OUTCOMES = new Set(['failed', 'disabled', 'scope-deny']);

// Provider errors may echo credentials; keep the reason for the log, never the secrets.
const redact = (error) => String(error?.message ?? error)
  .replace(/Bearer\s+\S+/gi, 'Bearer ***')
  .replace(/\b(?:sk|pk|rk|key)[-_][\w-]+/gi, '***')
  .replace(/[\w-]{24,}/g, '***')
  .replace(/\s+/g, ' ').slice(0, 200);

function mergeDeep(base, override) {
  if (!isRecord(base) || !isRecord(override)) return override === undefined ? base : override;
  const out = { ...base };
  for (const [key, value] of Object.entries(override)) out[key] = mergeDeep(base[key], value);
  return out;
}

/**
 * Applies `channels.kubik.tts` over the global `tts` block. The core telephony TTS helper takes no channel
 * context, so a Kubik-specific voice/persona is layered here the same way the core layers channel TTS config.
 */
export function kubikTtsConfig(cfg) {
  const own = cfg?.channels?.kubik?.tts;
  if (!isRecord(own)) return cfg;
  return { ...cfg, tts: mergeDeep(cfg.tts ?? {}, own) };
}

/** Converts a core telephony TTS result to 24 kHz s16le mono. */
export function toDevicePcm({ audioBuffer, outputFormat, sampleRate }) {
  const format = String(outputFormat ?? 'pcm').toLowerCase();
  if (/wav/.test(format) || parseWavSafe(audioBuffer)) {
    const wav = parseWav(audioBuffer);
    return normalizeDevicePcm(wav.pcm, wav.sampleRate);
  }
  if (/u-?law|mulaw|g711/.test(format)) return normalizeDevicePcm(mulawToPcm16(audioBuffer), Number(sampleRate) || 8000);
  if (/pcm|s16|raw|linear16/.test(format)) {
    const pcm = audioBuffer.subarray(0, audioBuffer.length - (audioBuffer.length % 2));
    const rate = Number(sampleRate) || Number(format.match(/(\d{4,5})/)?.[1]) || SAMPLE_RATE;
    return normalizeDevicePcm(pcm, rate);
  }
  throw new VoiceError(`unsupported telephony audio format ${format}`);
}
const normalizeDevicePcm = (pcm, rate) => rate === SAMPLE_RATE ? pcm : resamplePcm16(pcm, rate, SAMPLE_RATE);
function parseWavSafe(buffer) {
  try { return Boolean(parseWav(buffer)); } catch { return false; }
}

/**
 * Uses the Gateway's own configured speech stack:
 *   STT: runtime.mediaUnderstanding.transcribeAudioFile (tools.media.models entries with the audio capability)
 *   STT live: an optional per-PTT OpenAI transcription-only socket; final text enters the same agent route.
 *   TTS: same configured provider, streamed PCM where supported; designated batch route otherwise.
 */
export class OpenClawEngine {
  #chunks = [];
  #bytes = 0;
  #commit = null;
  #speech = null;
  #live = null;
  #capabilities = { canListen: false, canSpeak: false, stt: { available: false }, tts: { available: false } };
  constructor(voice, { log = () => {}, core, cfg, getConfig, transcribeTimeoutMs = 45_000, speechTimeoutMs = 45_000,
    tmpRoot = tmpdir(), speechSdk, liveOptions = {}, nativeOptions = {}, getVoice } = {}) {
    this.voice = voice;
    Object.assign(this, { log, core, transcribeTimeoutMs, speechTimeoutMs, tmpRoot, speechSdk, liveOptions, nativeOptions, getVoice });
    this.getConfig = getConfig ?? (() => cfg);
  }
  get connected() { return true; }
  get canListen() { return this.nativeVoice ? this.nativeVoice.available : this.#capabilities.canListen; }
  get voiceMode() { return this.voice.mode ?? 'classic'; }
  createVoiceTurn(options) {
    const Voice = this.voiceMode === 'live' ? LiveVoiceSession : NativeVoiceTurn;
    return new Voice({ config: this.nativeVoice, ...options, ...this.nativeOptions });
  }
  /** Without a configured provider and the runtime TTS API, replies are shown on the device screen. */
  get canSpeak() { return this.#capabilities.canSpeak; }

  /** Resolve cheap runtime/config facts before welcome; this never sends audio or invokes provider inference. */
  async refreshCapabilities({ agentId } = {}) {
    this.agentId = agentId;
    if (this.getVoice) this.voice = await this.getVoice();
    const cfg = this.getConfig();
    this.nativeVoice = await resolveNativeVoice({ cfg, voice: this.voice, agentId, sdk: this.nativeOptions.sdk });
    // Native input uses the host's voice provider; ordinary notifications still use configured TTS.
    const { stt, tts } = this.nativeVoice
      ? { stt: this.nativeVoice.stt, tts: configuredTts({ core: this.core, cfg, selection: this.voice.ttsSelection }) }
      : await resolveOpenClawVoiceCapabilities({ core: this.core, cfg, voice: this.voice, agentId });
    this.#capabilities = { canListen: stt.available, canSpeak: tts.available, stt, tts };
    return this.#capabilities;
  }
  beginTurn() {
    this.#chunks = []; this.#bytes = 0; this.#commit?.abort(); this.#live?.cancel();
    this.#live = this.voice.liveTranscription ? new LiveTranscription({ cfg: this.getConfig(),
      language: this.voice.language, timeoutMs: this.transcribeTimeoutMs,
      credential: (cfg, signal) => openAiCredential(cfg, signal, this.agentId ? this.core?.agent?.resolveAgentDir?.(cfg, this.agentId) : undefined),
      ...this.liveOptions }) : null;
  }
  append(pcm) {
    if (this.#live) { this.#live.append(pcm); return; }
    if (this.voice.liveTranscription) return;
    if (!pcm?.length || this.#bytes + pcm.length > MAX_TURN_BYTES) return;
    this.#chunks.push(Buffer.from(pcm)); this.#bytes += pcm.length;
  }

  async commit({ signal } = {}) {
    if (this.#live) {
      const live = this.#live;
      try { return await live.commit({ signal }); } finally { if (this.#live === live) this.#live = null; }
    }
    if (this.voice.liveTranscription) throw new VoiceError('Live transcription has no active PTT turn', { code: 'stt_failed' });
    const pcm = Buffer.concat(this.#chunks, this.#bytes);
    this.#chunks = []; this.#bytes = 0;
    if (pcm.length / BYTES_PER_MS < 100) return '';
    const controller = new AbortController();
    this.#commit = controller;
    const combined = signal ? AbortSignal.any([signal, controller.signal]) : controller.signal;
    const dir = await mkdtemp(join(this.tmpRoot, 'kubik-stt-'));
    try {
      const filePath = join(dir, 'turn.wav');
      await writeFile(filePath, pcmToWav(pcm), { mode: 0o600 });
      const request = this.core.mediaUnderstanding.transcribeAudioFile({
        filePath, cfg: selectedSttConfig(this.getConfig(), this.voice.sttSelection), mime: 'audio/wav', ...(this.voice.language ? { language: this.voice.language } : {}),
      });
      let result;
      try {
        result = await raceAbort(request, combined, this.transcribeTimeoutMs);
      } catch (error) {
        if (combined.aborted) throw abortError();
        if (error instanceof VoiceError) throw error;
        // Silence: the provider answered with an empty transcript and the core treats that as an error.
        if (/missing text|empty (?:transcript|text)|no speech/i.test(String(error?.message ?? error))) return '';
        this.log(`kubik: transcription error: ${redact(error)}`);
        throw new VoiceError('transcription failed', { code: 'stt_failed' });
      }
      const text = typeof result?.text === 'string' ? result.text.trim() : '';
      const outcome = result?.decision?.outcome;
      if (!text && FAILED_OUTCOMES.has(outcome)) {
        throw new VoiceError(`transcription ${outcome} (check tools.media.models)`, { code: 'stt_failed' });
      }
      if (!text && outcome && outcome !== 'success') this.log(`kubik: transcription returned no text (${outcome})`);
      return text;
    } finally {
      if (this.#commit === controller) this.#commit = null;
      await rm(dir, { recursive: true, force: true }).catch(() => {});
    }
  }

  async speak(text, { onAudio, signal } = {}) {
    if (!this.canSpeak) throw new VoiceError('OpenClaw has no configured TTS provider');
    const controller = new AbortController();
    this.#speech = controller;
    const combined = signal ? AbortSignal.any([signal, controller.signal]) : controller.signal;
    try {
      try {
        await streamDeviceSpeech({ text, cfg: kubikTtsConfig(this.getConfig()), pitch: this.voice.pitch, selection: this.voice.ttsSelection,
          signal: combined, timeoutMs: this.speechTimeoutMs, onAudio, loadSdk: this.speechSdk, normalizePcm: toDevicePcm });
      } catch (error) {
        if (combined.aborted) return { cancelled: true };
        if (error instanceof VoiceError) throw error;
        throw new VoiceError('speech synthesis failed');
      }
      return { cancelled: combined.aborted };
    } finally {
      if (this.#speech === controller) this.#speech = null;
    }
  }

  cancel() { this.#speech?.abort(); }
  cancelTranscription() { this.#chunks = []; this.#bytes = 0; this.#commit?.abort(); this.#live?.cancel(); }
  close() { this.#speech?.abort(); this.#commit?.abort(); this.#live?.cancel(); }
}

const AUTO_STT_PROVIDERS = Object.freeze(['groq', 'openai', 'xai', 'deepgram', 'google', 'senseaudio', 'elevenlabs', 'mistral']);

export async function resolveOpenClawVoiceCapabilities({ core, cfg, voice = {}, agentId,
  env = process.env, platform = process.platform, arch = process.arch } = {}) {
  const stt = await configuredStt({ core, cfg, voice, agentId, env, platform, arch });
  return { stt, tts: configuredTts({ core, cfg, selection: voice.ttsSelection }) };
}

async function configuredStt({ core, cfg, voice, agentId, env, platform, arch }) {
  const unavailable = { available: false };
  if (cfg?.tools?.media?.audio?.enabled === false || typeof core?.mediaUnderstanding?.transcribeAudioFile !== 'function') return unavailable;
  if (voice?.liveTranscription === true) return liveStt(core, cfg, agentId, unavailable, env);
  const rows = configuredAudioRows(cfg);
  if (voice.sttSelection) {
    const row = rows.find(row => row.provider === voice.sttSelection.provider && row.model === voice.sttSelection.model);
    return row ? await explicitAudioRow(core, cfg, agentId, row, env) ?? unavailable : unavailable;
  }
  if (rows.length) return await explicitStt(core, cfg, agentId, rows.slice(0, 128), env, unavailable);
  return await automaticStt(core, cfg, agentId, { env, platform, arch }) ?? unavailable;
}

async function liveStt(core, cfg, agentId, unavailable, env) {
  try {
    const agentDir = agentId ? core.agent?.resolveAgentDir?.(cfg, agentId) : undefined;
    await resolveLiveCredential({ cfg, agentDir, env, resolveAuth: (params) => core.modelAuth.resolveApiKeyForProvider(params) });
    return { available: true, provider: 'openai', model: 'gpt-live-transcribe' };
  } catch { return unavailable; }
}

async function explicitStt(core, cfg, agentId, rows, env, unavailable) {
  for (const row of rows) {
    const selected = await explicitAudioRow(core, cfg, agentId, row, env);
    if (selected) return selected;
  }
  return unavailable;
}

async function explicitAudioRow(core, cfg, agentId, row, env) {
  if (row?.type === 'cli' && row.command && await executable(row.command, env)) {
    return { available: true, provider: 'local', model: row.command };
  }
  if (!row?.provider) return null;
  const auth = await resolveProviderAuth(core, row.provider, cfg, agentId, row.profile, row.preferredProfile);
  return auth?.apiKey || auth?.mode === 'aws-sdk'
    ? { available: true, provider: row.provider, model: row.model ?? null } : null;
}

async function automaticStt(core, cfg, agentId, options) {
  for (const provider of AUTO_STT_PROVIDERS) {
    const auth = await resolveProviderAuth(core, provider, cfg, agentId);
    if (auth?.apiKey || auth?.mode === 'aws-sdk') return { available: true, provider, model: null };
  }
  return autoLocalStt(options);
}

export function configuredAudioRows(cfg) {
  return (cfg?.tools?.media?.models ?? []).filter(row => row?.capabilities?.includes('audio'));
}

/** Preserve model array indices: the SDK uses them to resolve secret ownership. */
export function selectedSttConfig(cfg, selection) {
  if (!selection) return cfg;
  const rows = cfg?.tools?.media?.models ?? [];
  const index = rows.findIndex(row => row.provider === selection.provider && row.model === selection.model && row.capabilities?.includes('audio'));
  if (index < 0) throw new VoiceError('Selected input model is unavailable', { code: 'stt_failed' });
  const models = rows.map((row, at) => at === index ? row : {});
  return { ...cfg, tools: { ...cfg.tools, media: { ...cfg.tools.media, models } } };
}

async function autoLocalStt({ env = process.env, platform = process.platform, arch = process.arch } = {}) {
  const has = (name) => findExecutable(name, env);
  const [pythonWhisper, whisperCpp, sherpa, parakeet] = await Promise.all([
    has('whisper'), has('whisper-cli'), has('sherpa-onnx-offline'), has('parakeet-mlx'),
  ]);
  const sherpaDir = env.SHERPA_ONNX_MODEL_DIR?.trim();
  if (sherpa && sherpaDir && await hasAll(expandHome(sherpaDir, env.HOME), ['tokens.txt', 'encoder.onnx', 'decoder.onnx', 'joiner.onnx'])) {
    return { available: true, provider: 'local', model: 'sherpa-onnx-offline' };
  }
  if (whisperCpp && await whisperCppModel(env)) return { available: true, provider: 'local', model: 'whisper-cli' };
  if (platform === 'darwin' && arch === 'arm64' && parakeet) return { available: true, provider: 'local', model: 'parakeet-mlx' };
  if (pythonWhisper) return { available: true, provider: 'local', model: 'whisper' };
  return null;
}

async function findExecutable(name, env) {
  const value = expandHome(String(name ?? '').trim(), env.HOME);
  if (!value) return false;
  const dirs = value.includes('/') || value.includes('\\') ? [''] : String(env.PATH ?? '').split(delimiter).slice(0, 256);
  for (const dir of dirs) {
    const candidate = dir ? join(expandHome(dir, env.HOME), value) : expandHome(value, env.HOME);
    try { await access(candidate, constants.X_OK); return true; } catch { /* Keep checking PATH. */ }
  }
  return false;
}

function expandHome(value, home) {
  return value.replace(/^~(?=$|[\\/])/, home ?? '~');
}

async function executable(command, env) {
  return findExecutable(command, env);
}

async function hasAll(directory, names) {
  for (const name of names) {
    try { await access(join(directory, name)); } catch { return false; }
  }
  return true;
}

async function whisperCppModel(env) {
  const configured = expandHome(env.WHISPER_CPP_MODEL?.trim() ?? '', env.HOME);
  if (configured) {
    try { await access(configured); return true; } catch { return false; }
  }
  for (const directory of ['/opt/homebrew/share/whisper-cpp', '/usr/local/share/whisper-cpp', '/usr/share/whisper-cpp']) {
    try {
      if ((await readdir(directory)).some((name) => name.startsWith('ggml-') && name.endsWith('.bin'))) return true;
    } catch { /* This optional local model directory is absent. */ }
  }
  return false;
}

async function resolveProviderAuth(core, provider, cfg, agentId, profileId, preferredProfile) {
  if (typeof core?.modelAuth?.resolveApiKeyForProvider !== 'function') return null;
  try {
    const agentDir = agentId ? core.agent?.resolveAgentDir?.(cfg, agentId) : undefined;
    return await core.modelAuth.resolveApiKeyForProvider({ provider, cfg, ...(agentDir ? { agentDir } : {}),
      ...(profileId ? { profileId } : {}), ...(preferredProfile ? { preferredProfile } : {}) });
  } catch { return null; }
}

function selectedTtsMetadata(implementation, id, resolved, selection) {
  if (selection && (selection.provider !== id || !implementation.models?.includes(selection.model))) return { available: false };
  return { available: true, provider: id, model: selection?.model ?? resolved.model ?? resolved.modelId ?? null };
}

function configuredTts({ core, cfg, selection }) {
  if (typeof core?.tts?.textToSpeechTelephony !== 'function') return { available: false };
  try {
    const effective = kubikTtsConfig(cfg);
    const resolved = resolveTtsConfig(effective, { channelId: 'kubik', accountId: 'default' });
    const id = getTtsProvider(resolved, resolveTtsPrefsPath(resolved));
    const implementation = getSpeechProvider(id, effective);
    if (!implementation || !isTtsProviderConfigured(resolved, implementation, effective)) return { available: false };
    const supportsDevicePcm = id === 'openai'
      || (id === 'elevenlabs' && typeof implementation.streamSynthesize === 'function')
      || typeof implementation.synthesizeTelephony === 'function';
    if (!supportsDevicePcm) return { available: false };
    return selectedTtsMetadata(implementation, id, getResolvedSpeechProviderConfig(resolved, id, effective), selection);
  } catch { return { available: false }; }
}

function abortError() {
  const error = new Error('aborted'); error.name = 'AbortError'; return error;
}

/** Resolves with `promise`, or rejects when `signal` aborts or `ms` elapses (the underlying call keeps running). */
function raceAbort(promise, signal, ms) {
  if (signal.aborted) { promise.catch(() => {}); return Promise.reject(abortError()); }
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { cleanup(); reject(new VoiceError('voice request timed out')); }, ms);
    const onAbort = () => { cleanup(); reject(abortError()); };
    const cleanup = () => { clearTimeout(timer); signal.removeEventListener('abort', onAbort); };
    signal.addEventListener('abort', onAbort, { once: true });
    promise.then((value) => { cleanup(); resolve(value); }, (error) => { cleanup(); reject(error); });
  });
}
