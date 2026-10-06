import { createVoiceControl } from './voice-control.js';
import { loadPreparedModelCatalog, resolveAllowedModelRef, resolveDefaultModelForAgent } from 'openclaw/plugin-sdk/agent-runtime';
import { deviceRoute, dispatchModelSelection } from './inbound.js';
import { resolveOpenClawVoiceCapabilities } from './engines/openclaw.js';
import { resolveNativeVoice } from './native-voice.js';

const MAX_MODELS = 128;
const MODEL_TTL_MS = 15_000;
const CACHE_LIMIT = 8;
const CANONICAL_MODEL = /^[^/\s]+\/[^\s]+$/;

/** Session-local model selection and truthful configured speech metadata for the authenticated device route. */
export function createAgentControl({ core, sdk, cfg, account, log = () => {}, info = () => {}, modelRuntime = {},
  commandDispatch = dispatchModelSelection, selectionTimeoutMs = 6_500 } = {}) {
  const runtime = { loadPreparedModelCatalog, resolveAllowedModelRef, resolveDefaultModelForAgent, ...modelRuntime };
  const catalogs = new Map();
  const voiceCapabilities = new Map();
  const routeFor = (device) => deviceRoute({ core, cfg, account, deviceId: device.id });
  const agentId = (device) => routeFor(device).route.agentId;
  const voiceControl = createVoiceControl({ core, cfg, account, agentId });
  return {
    agentId,
    voiceSettings: voiceControl.settings,
    async options(device, target = 'agent') {
      if (target !== 'agent') return voiceControl.options(device, target);
      const { cfg: routeConfig, route } = routeFor(device);
      const [models, voice] = await Promise.all([loadCatalog(routeConfig, route.agentId), capabilities(routeConfig, route.agentId, device)]);
      const entry = readSession(route.agentId, route.sessionKey);
      return { model: effectiveModel(runtime, routeConfig, route.agentId, entry), models, stt: voice.stt, tts: voice.tts };
    },
    async selectModel(device, id, target = 'agent') {
      if (target !== 'agent') {
        await voiceControl.selectModel(device, target, id);
        voiceCapabilities.clear(); return;
      }
      if (typeof id !== 'string' || id.length > 160 || !CANONICAL_MODEL.test(id)) throw controlError('invalid_model');
      const { cfg: routeConfig, route } = routeFor(device);
      info(`kubik: model control ${device.id} selection-start`);
      const catalogStarted = Date.now();
      info(`kubik: model control ${device.id} catalog-start`);
      const models = await loadCatalog(routeConfig, route.agentId);
      info(`kubik: model control ${device.id} catalog ${Date.now() - catalogStarted}ms (${models.length} entries)`);
      if (!models.some((model) => model.id === id)) throw controlError('invalid_model');
      if (typeof core?.agent?.session?.getSessionEntry !== 'function') throw controlError('unsupported');
      if (effectiveModel(runtime, routeConfig, route.agentId, readSession(route.agentId, route.sessionKey)) === id) {
        info(`kubik: model control ${device.id} already-selected`);
        return;
      }
      try {
        const commandStarted = Date.now();
        await commandDispatch({ core, sdk, account, cfg: routeConfig, device, modelId: id, log, info,
          ackTimeoutMs: selectionTimeoutMs,
          isPersisted: () => effectiveModel(runtime, routeConfig, route.agentId, readSession(route.agentId, route.sessionKey)) === id });
        info(`kubik: model control ${device.id} native command ${Date.now() - commandStarted}ms`);
      } catch (error) {
        const code = ['unsupported', 'invalid_model', 'denied', 'unavailable', 'timeout'].includes(error?.code) ? error.code : 'unavailable';
        log(`kubik: model command for ${device.id} failed (${code})`);
        throw controlError(code);
      }
      const readbackStarted = Date.now();
      info(`kubik: model control ${device.id} read-back-start`);
      const after = readSession(route.agentId, route.sessionKey);
      info(`kubik: model control ${device.id} read-back ${Date.now() - readbackStarted}ms`);
      if (effectiveModel(runtime, routeConfig, route.agentId, after) !== id) {
        log(`kubik: model command for ${device.id} returned without persisting ${id}`);
        throw controlError('unavailable');
      }
      if (!after) throw controlError('unavailable');
    },
  };

  async function loadCatalog(routeConfig, id) {
    const now = Date.now();
    const cached = catalogs.get(id);
    if (cached && cached.expires > now) return cached.promise;
    const promise = readCatalog(runtime, routeConfig, id);
    catalogs.delete(id);
    catalogs.set(id, { expires: now + MODEL_TTL_MS, promise });
    while (catalogs.size > CACHE_LIMIT) catalogs.delete(catalogs.keys().next().value);
    promise.catch(() => { if (catalogs.get(id)?.promise === promise) catalogs.delete(id); });
    return promise;
  }

  async function capabilities(routeConfig, id, device) {
    const cacheKey = `${id}:${device.id}:${device.fingerprint}`;
    const cached = voiceCapabilities.get(cacheKey);
    if (cached && cached.expires > Date.now()) return cached.value;
    const voice = await voiceControl.settings(device);
    const native = await resolveNativeVoice({ cfg: routeConfig, voice, agentId: id });
    const value = native ?? (account?.voice?.provider === 'openclaw'
      ? await resolveOpenClawVoiceCapabilities({ core, cfg: routeConfig, voice, agentId: id })
      : customVoiceCapabilities(account?.voice));
    const normalized = { stt: safeSpeechMetadata(value?.stt), tts: safeSpeechMetadata(value?.tts) };
    voiceCapabilities.set(cacheKey, { expires: Date.now() + MODEL_TTL_MS, value: normalized });
    while (voiceCapabilities.size > CACHE_LIMIT) voiceCapabilities.delete(voiceCapabilities.keys().next().value);
    return normalized;
  }

  function readSession(id, key) {
    if (typeof core?.agent?.session?.getSessionEntry !== 'function') return undefined;
    return core.agent.session.getSessionEntry({ agentId: id, sessionKey: key, readConsistency: 'latest' });
  }

}

async function readCatalog(runtime, cfg, agentId) {
  assertCatalogRuntime(runtime);
  const catalog = await loadCatalogSnapshot(runtime, cfg, agentId);
  const defaultRef = runtime.resolveDefaultModelForAgent({ cfg, agentId });
  const rows = new Map();
  for (const row of (catalog ?? []).slice(0, 512)) addCatalogRow({ rows, row, runtime, cfg, catalog, defaultRef, agentId });
  return [...rows.values()];
}

function assertCatalogRuntime(runtime) {
  if (typeof runtime.loadPreparedModelCatalog !== 'function' || typeof runtime.resolveAllowedModelRef !== 'function'
    || typeof runtime.resolveDefaultModelForAgent !== 'function') throw controlError('unsupported');
}

async function loadCatalogSnapshot(runtime, cfg, agentId) {
  return runtime.loadPreparedModelCatalog({ config: cfg, agentId, readOnly: true,
    providerDiscoveryProviderIds: [], scopedLiveProviderDiscovery: false });
}

function addCatalogRow({ rows, row, runtime, cfg, catalog, defaultRef, agentId }) {
  if (rows.size >= MAX_MODELS) return;
  const raw = canonicalRow(row);
  if (!raw) return;
  const resolved = resolveModelRef(runtime, { cfg, catalog, raw, defaultRef, agentId });
  const provider = resolved?.ref?.provider, model = resolved?.ref?.model;
  const id = provider && model ? `${provider}/${model}` : '';
  if (resolved?.error || id.length > 160 || !CANONICAL_MODEL.test(id) || rows.has(id)) return;
  rows.set(id, { id, label: safeLabel(row.name || row.alias || id) });
}

function resolveModelRef(runtime, { cfg, catalog, raw, defaultRef, agentId }) {
  try {
    return runtime.resolveAllowedModelRef({ cfg, catalog, raw, defaultProvider: defaultRef.provider,
      defaultModel: defaultRef.model, agentId });
  } catch { return null; }
}

function canonicalRow(row) {
  const provider = typeof row?.provider === 'string' ? row.provider.trim() : '';
  const model = typeof row?.id === 'string' ? row.id.trim() : '';
  return provider && model && !/[\p{C}\s/]/u.test(provider) && !/[\p{C}\s]/u.test(model) ? `${provider}/${model}` : '';
}

function safeLabel(value) { return String(value).replace(/[\p{C}]/gu, '').trim().slice(0, 80) || 'Model'; }

function safeModelId(value) {
  const id = typeof value === 'string' ? value.replace(/[\p{C}]/gu, '').trim() : '';
  return id.length <= 160 && CANONICAL_MODEL.test(id) ? id : '';
}

function safeSpeechMetadata(value) {
  const metadata = { available: value?.available === true };
  if (!metadata.available) return metadata;
  const provider = safeText(value.provider, 40), model = safeText(value.model, 160);
  if (provider) metadata.provider = provider;
  if (model) metadata.model = model;
  return metadata;
}

function safeText(value, max) {
  return typeof value === 'string' ? value.replace(/[\p{C}]/gu, '').trim().slice(0, max) : '';
}

function defaultModel(runtime, cfg, agentId) {
  const model = runtime.resolveDefaultModelForAgent({ cfg, agentId });
  return model?.provider && model?.model ? `${model.provider}/${model.model}` : '';
}

function effectiveModel(runtime, cfg, agentId, entry) {
  const fallback = runtime.resolveDefaultModelForAgent({ cfg, agentId });
  const selected = isDefaultSelection(entry) ? fallback : storedModelSelection(entry, fallback);
  return safeModelId(modelReference(selected));
}

function isDefaultSelection(entry) {
  return entry?.modelOverrideSource === 'default' || entry?.modelOverrideSource === 'auto';
}

function storedModelSelection(entry, fallback) {
  return { provider: entry?.providerOverride ?? entry?.modelProvider ?? fallback?.provider,
    model: entry?.modelOverride ?? entry?.model ?? fallback?.model };
}

function modelReference(selection) {
  return selection?.provider && selection?.model ? `${selection.provider}/${selection.model}` : '';
}

function customVoiceCapabilities(voice) {
  const available = voice?.provider === 'openai-http' && Boolean(voice.apiKey);
  return {
    stt: { available, ...(available ? { provider: 'openai', model: voice.transcribeModel ?? null } : {}) },
    tts: { available, ...(available ? { provider: 'openai', model: voice.ttsModel ?? null } : {}) },
  };
}

function controlError(code) { return Object.assign(new Error(code), { code }); }
