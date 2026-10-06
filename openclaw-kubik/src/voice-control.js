import { join } from 'node:path';
import { createVoiceModelStore } from './voice-model-store.js';
import { getRealtimeVoiceProvider } from 'openclaw/plugin-sdk/realtime-voice';
import { getSpeechProvider } from 'openclaw/plugin-sdk/speech';
import { resolveNativeVoice } from './native-voice.js';
import { resolveOpenClawVoiceCapabilities, configuredAudioRows } from './engines/openclaw.js';

const fail = (code) => { throw Object.assign(new Error(code), { code }); };
const validId = (id) => typeof id === 'string' && id.length <= 160 && /^[^/\s]+\/[^\s\p{C}]+$/u.test(id);
const ref = (metadata) => metadata?.provider && metadata?.model ? `${metadata.provider}/${metadata.model}` : '';
const split = (id) => { const slash = id.indexOf('/'); return { provider: id.slice(0, slash), model: id.slice(slash + 1) }; };
const MODES = ['classic', 'realtime', 'live'];
const NAMES = ['STT', 'Realtime', 'GPT Live'];
const slot = (voice, target) => target === 'voice' ? voice.mode : target;

/** Persistent, device-scoped speech choices. Catalogs are SDK/config facts, never paid probes. */
export function createVoiceControl({ core, cfg, account, agentId, runtime = {} }) {
  const nativeProvider = runtime.getRealtimeVoiceProvider ?? getRealtimeVoiceProvider;
  const speechProvider = runtime.getSpeechProvider ?? getSpeechProvider;
  const native = runtime.resolveNativeVoice ?? resolveNativeVoice;
  const classic = runtime.resolveOpenClawVoiceCapabilities ?? resolveOpenClawVoiceCapabilities;
  let store;
  const storage = () => store ??= createVoiceModelStore(join(core.state.resolveStateDir(process.env), 'kubik'));
  const key = (device) => JSON.stringify([account.accountId, device.id, device.fingerprint]);
  async function settings(device) {
    const saved = storage().lookup(key(device));
    const defaultMode = account.voice?.mode ?? 'classic';
    const voice = { ...account.voice, mode: saved.mode ?? defaultMode };
    if (voice.mode !== defaultMode) delete voice.model;
    if (voice.mode !== 'classic') voice.liveTranscription = false;
    const selected = saved[voice.mode === 'classic' ? 'stt' : voice.mode];
    if (validId(selected)) {
      if (voice.mode === 'realtime' || voice.mode === 'live') voice.model = split(selected).model;
      else voice.sttSelection = split(selected);
    }
    if (validId(saved.tts)) voice.ttsSelection = split(saved.tts);
    return voice;
  }
  async function options(device, target) {
    if (!['mode', 'voice', 'stt', 'tts'].includes(target)) fail('unsupported');
    if (account.voice?.provider !== 'openclaw') fail('unsupported');
    const voice = await settings(device);
    const id = agentId(device);
    if (target === 'mode') return modeOptions(voice, id);
    if ((voice.mode !== 'classic') !== (target === 'voice')) fail('unsupported');
    return modelOptions(voice, id, target);
  }
  async function modelOptions(voice, id, target) {
    const shared = voice.mode === 'classic' ? null : await modeCapabilities(voice, id);
    const capabilities = shared ?? await classic({ core, cfg, voice, agentId: id });
    const baseVoice = { ...voice, model: undefined, sttSelection: undefined, ttsSelection: undefined };
    const ready = shared ? await modeCapabilities(baseVoice, id)
      : await classic({ core, cfg, voice: baseVoice, agentId: id });
    const metadata = capabilities[target === 'voice' ? 'stt' : target];
    const configured = ready[target === 'voice' ? 'stt' : target];
    const candidates = await catalog(voice, target, shared, ready, configured, id);
    const models = [...new Set(candidates.filter(validId))].slice(0, 128).map(id => ({ id, label: split(id).model.slice(0, 80) }));
    const selected = target === 'tts' ? voice.ttsSelection : voice.sttSelection;
    return { model: shared && voice.model ? `openai/${voice.model}` : ref(selected ?? metadata),
      models, stt: capabilities.stt, tts: capabilities.tts };
  }
  async function modeCapabilities(voice, id) {
    if (voice.mode === 'classic') return classic({ core, cfg, voice, agentId: id });
    if (nativeProvider('openai', cfg)) {
      try { return await native({ cfg, voice, agentId: id }); }
      catch { /* Invalid native setup must not hide a working STT mode. */ }
    }
    return { stt: { available: false }, tts: { available: false } };
  }
  async function modeOptions(voice, id) {
    const models = [];
    for (const [index, mode] of MODES.entries()) {
      const candidate = { ...voice, mode, model: undefined, sttSelection: undefined, ttsSelection: undefined };
      const capabilities = await modeCapabilities(candidate, id);
      models.push({ id: mode, label: NAMES[index], available: Boolean(capabilities.stt.available) });
    }
    const current = await modeCapabilities(voice, id);
    return { model: voice.mode, models, stt: current.stt, tts: current.tts };
  }
  async function inputCatalog(voice, id) {
    const candidates = [];
    if (voice.liveTranscription) return candidates;
    for (const row of configuredAudioRows(cfg).slice(0, 128)) {
      if (!row.provider || !row.model || row.type === 'cli') continue;
      const resolved = await classic({ core, cfg, voice: { ...voice, sttSelection: row }, agentId: id });
      if (resolved.stt.available) candidates.push(`${row.provider}/${row.model}`);
    }
    return candidates;
  }
  async function catalog(voice, target, shared, ready, configured, id) {
    if (shared) return ready.available ? (nativeProvider('openai', cfg)?.models ?? [])
      .filter(model => (voice.mode === 'live') === model.startsWith('gpt-live-')).map(model => `openai/${model}`) : [];
    if (target === 'stt') return inputCatalog(voice, id);
    if (!configured?.available) return [];
    const provider = speechProvider(configured.provider, cfg);
    return provider?.resolveTalkOverrides ? (provider.models ?? []).map(model => `${provider.id}/${model}`) : [];
  }
  async function selectModel(device, target, id) {
    if (target === 'mode' ? !MODES.includes(id) : !validId(id)) fail('invalid_model');
    const available = await options(device, target);
    const choice = available.models.find(row => row.id === id);
    if (!choice) fail('invalid_model');
    if (choice.available === false) fail('unavailable');
    await storage().select(key(device), target === 'mode' ? 'mode' : slot(await settings(device), target), id);
  }
  return { settings, options, selectModel };
}
