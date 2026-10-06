import { closeSync, constants, fstatSync, lstatSync, openSync, readSync } from 'node:fs';
import { isAbsolute } from 'node:path';
import { LAN_PORT } from './protocol.js';

export const CHANNEL_ID = 'kubik';
export const DEFAULT_ACCOUNT_ID = 'default';
export const DEFAULT_VOICE = Object.freeze({
  provider: 'openclaw', mode: 'classic', baseUrl: 'https://api.openai.com/v1', voice: 'marin',
  transcribeModel: 'gpt-4o-transcribe', language: 'ru', ttsModel: 'gpt-4o-mini-tts', liveTranscription: false,
});
export const DEVICE_ID_PATTERN = '^[a-z0-9][a-z0-9_-]{0,63}$';
const DEVICE_ID_RE = new RegExp(DEVICE_ID_PATTERN);
const SECRET_FILE_MAX_BYTES = 64 * 1024;

export const isRecord = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
/** `kubik:kubik-b6c634`, `kubik-b6c634` and ` Kubik-B6C634 ` all name the same device. */
export const normalizeDeviceId = (value) => String(value ?? '').trim().replace(/^kubik:/i, '').toLowerCase();
export const isDeviceId = (value) => typeof value === 'string' && DEVICE_ID_RE.test(value);

const string = { type: 'string' };
const boolean = { type: 'boolean' };
const deviceSchema = {
  type: 'object', additionalProperties: false,
  properties: { name: string, enabled: boolean },
};
const voiceSchema = {
  type: 'object', additionalProperties: false,
  allOf: [{
    if: { required: ['mode'], properties: { mode: { enum: ['realtime', 'live'] } } },
    then: { properties: { provider: { const: 'openclaw' }, liveTranscription: { const: false } } },
  }],
  properties: {
    provider: { type: 'string', enum: ['openai-http', 'openclaw'] },
    mode: { type: 'string', enum: ['classic', 'realtime', 'live'] },
    baseUrl: string, apiKey: string, apiKeyFile: string, voice: string,
    transcribeModel: string, language: string, ttsModel: string, speechInstructions: string,
    pitch: { type: 'number', minimum: -12, maximum: 12 },
    liveTranscription: boolean,
  },
};
export const channelSchema = {
  type: 'object', additionalProperties: false,
  properties: {
    enabled: boolean,
    name: string,
    listen: { type: 'object', additionalProperties: false, properties: {
      enabled: boolean, public: boolean, port: { type: 'integer', minimum: 1, maximum: 65535 },
    } },
    devices: { type: 'object', propertyNames: { pattern: DEVICE_ID_PATTERN }, additionalProperties: deviceSchema },
    voice: voiceSchema,
    allowInsecureBaseUrl: boolean,
    volume: { type: 'integer', minimum: 0, maximum: 100 },
    defaultTo: string,
    // Text on Kubik's screen: "auto" = [[show]] blocks and replies that could not be spoken; "always" = every reply is also shown.
    text: { type: 'string', enum: ['auto', 'always'] },
    // Channel-level override of the core `tts` block (auto mode, provider, voice); read by OpenClaw core and
    // by the `openclaw` voice engine. Shape is owned by core, so it is not validated here.
    tts: { type: 'object', additionalProperties: {} },
  },
};
export const uiHints = {
  'listen.enabled': { label: 'LAN TLS listener and discovery (false on hosts without devices in their LAN)' },
  'listen.public': { label: 'Also accept devices from public addresses on the TLS port (VPS without a domain; discovery stays private)' },
  'listen.port': { label: 'LAN TLS port for devices (discovery replies carry it)', placeholder: '18790' },
  'devices.*.enabled': { label: 'Allow this device (false revokes it, including paired keys)' },
  'voice.baseUrl': { label: 'OpenAI-compatible API URL', placeholder: 'https://api.openai.com/v1' },
  'voice.apiKey': { label: 'OpenAI API key', sensitive: true },
  'voice.apiKeyFile': { label: 'OpenAI API key file' },
  'voice.pitch': { label: 'Voice pitch shift, semitones (openclaw engine; +6 = cartoon character)' },
  'voice.provider': { label: 'Voice engine: openclaw = configured Gateway STT/TTS (default), openai-http = an OpenAI-compatible /audio API' },
  'voice.mode': { label: 'Classic STT/TTS, OpenAI Realtime, or GPT-Live voice of the same agent (native modes require Gateway OpenAI API-key auth)' },
  'voice.liveTranscription': { label: 'Stream microphone audio to OpenAI gpt-live-transcribe instead of batch transcription; requires OpenAI API-key auth' },
  tts: { label: 'Kubik TTS override (same shape as the top-level tts block)' },
  allowInsecureBaseUrl: { label: 'Allow http:// voice provider (local mock only)' },
};

/** Reads a small secret file without following symlinks. Never includes the content in errors. */
export function readSecretFile(path, label = 'secret file') {
  if (!isAbsolute(path)) throw new Error(`Kubik ${label} must be an absolute path`);
  let fd;
  try {
    if (lstatSync(path).isSymbolicLink()) throw new Error('symlink');
    fd = openSync(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    const stat = fstatSync(fd);
    if (!stat.isFile() || stat.size > SECRET_FILE_MAX_BYTES) throw new Error('invalid');
    const buffer = Buffer.alloc(SECRET_FILE_MAX_BYTES + 1);
    let bytes = 0;
    while (bytes < buffer.length) {
      const count = readSync(fd, buffer, bytes, buffer.length - bytes, bytes);
      if (!count) break;
      bytes += count;
    }
    if (bytes > SECRET_FILE_MAX_BYTES) throw new Error('invalid');
    return buffer.toString('utf8', 0, bytes).trim();
  } catch {
    throw new Error(`Cannot read Kubik ${label}`);
  } finally {
    if (fd !== undefined) closeSync(fd);
  }
}

export function normalizeBaseUrl(input, allowInsecure = false) {
  let url;
  try { url = new URL(input); } catch { throw new Error('voice.baseUrl must be an absolute URL'); }
  if (url.username || url.password || url.search || url.hash || !['https:', 'http:'].includes(url.protocol)) {
    throw new Error('voice.baseUrl must be HTTP(S), without credentials, query or fragment');
  }
  if (url.protocol === 'http:' && !allowInsecure) {
    throw new Error('http:// sends the voice API key in cleartext; use HTTPS or set allowInsecureBaseUrl (local mock only)');
  }
  return url.href.replace(/\/+$/, '');
}

export function sectionOf(cfg) { return cfg?.channels?.[CHANNEL_ID] ?? {}; }

function resolveDevices(section) {
  const devices = new Map();
  for (const [id, device] of Object.entries(section.devices ?? {})) {
    if (!isRecord(device) || Object.keys(device).some((key) => !['name', 'enabled'].includes(key))) {
      throw new Error(`Kubik devices.${id} contains an unsupported setting`);
    }
    devices.set(id, { id, name: device.name?.trim() || id, enabled: device.enabled !== false });
  }
  return devices;
}

function resolveVoice(section, env, readSecrets) {
  const own = { ...section.voice };
  const unsupported = Object.keys(own).find((key) => !(key in voiceSchema.properties));
  if (unsupported) throw new Error(`Kubik voice.${unsupported} is not supported`);
  const voice = { ...DEFAULT_VOICE, ...Object.fromEntries(Object.entries(own).filter(([, v]) => v !== undefined && v !== '')) };
  delete voice.apiKey; delete voice.apiKeyFile;
  if (!voiceSchema.properties.provider.enum.includes(voice.provider)) throw new Error(`Kubik voice.provider "${voice.provider}" is not supported`);
  validateVoiceMode(voice);
  if (own.apiKey && own.apiKeyFile) throw new Error('Kubik voice: set apiKey or apiKeyFile, not both');
  if (voice.liveTranscription && voice.provider !== 'openclaw') throw new Error('Kubik voice.liveTranscription requires voice.provider "openclaw"');
  if (voice.provider === 'openclaw') {
    if (own.apiKey || own.apiKeyFile) throw new Error('Kubik voice.provider "openclaw" uses the Gateway STT/TTS; remove voice.apiKey/apiKeyFile');
    voice.apiKeySource = 'openclaw';
    Object.defineProperty(voice, 'apiKey', { value: '', enumerable: false });
    return voice;
  }
  const { apiKey, apiKeySource } = resolveCustomVoice(own, env, readSecrets);
  voice.baseUrl = normalizeBaseUrl(voice.baseUrl, section.allowInsecureBaseUrl === true);
  voice.apiKeySource = apiKeySource;
  // Non-enumerable: the key cannot leak through JSON.stringify/structuredClone of a status snapshot.
  Object.defineProperty(voice, 'apiKey', { value: apiKey, enumerable: false });
  return voice;
}

function validateVoiceMode(voice) {
  if (!voiceSchema.properties.mode.enum.includes(voice.mode)) throw new Error('Kubik voice.mode is not supported');
  if (voice.mode !== 'classic' && (voice.provider !== 'openclaw' || voice.liveTranscription)) {
    throw new Error('Kubik native voice modes require openclaw and liveTranscription disabled');
  }
}

function resolveCustomVoice(own, env, readSecrets) {
  let apiKey = own.apiKey?.trim() || '';
  let apiKeySource = apiKey ? 'config' : 'none';
  if (own.apiKeyFile) {
    apiKeySource = 'file';
    if (!isAbsolute(own.apiKeyFile)) throw new Error('Kubik voice.apiKeyFile must be an absolute path');
    if (readSecrets) apiKey = readSecretFile(own.apiKeyFile, 'voice.apiKeyFile');
  } else if (!apiKey && env.OPENAI_API_KEY?.trim()) {
    apiKey = readSecrets ? env.OPENAI_API_KEY.trim() : '';
    apiKeySource = 'env';
  }
  return { apiKey, apiKeySource };
}

/**
 * The channel has one implicit `default` account; devices are peers inside it. The Gateway validates
 * `channels.kubik` against the manifest schema (generated from `channelSchema`) on every config write and load.
 * `readSecrets: false` never touches OpenAI key files or the environment key (setup/status paths).
 */
export function resolveAccount(cfg, requestedId, { env = process.env, readSecrets = true } = {}) {
  const section = sectionOf(cfg);
  const accountId = requestedId?.trim() || DEFAULT_ACCOUNT_ID;
  if (accountId !== DEFAULT_ACCOUNT_ID) throw new Error('Kubik has a single implicit "default" account; add devices under channels.kubik.devices');
  const listen = { enabled: section.listen?.enabled !== false, public: section.listen?.public === true, port: section.listen?.port ?? LAN_PORT };
  const devices = resolveDevices(section);
  const voice = resolveVoice(section, env, readSecrets);
  // Devices are optional: devices pair themselves through the OpenClaw pairing store.
  const configured = isRecord(cfg?.channels?.[CHANNEL_ID]) && section.enabled !== false;
  return {
    accountId, name: section.name, enabled: section.enabled !== false, configured,
    listen, devices, voice, allowInsecureBaseUrl: section.allowInsecureBaseUrl === true,
    volume: section.volume, text: section.text ?? 'auto', defaultTo: section.defaultTo ? normalizeDeviceId(section.defaultTo) : undefined,
    config: section,
  };
}

export function inspectAccount(cfg, accountId) {
  const account = resolveAccount(cfg, accountId, { readSecrets: false });
  return { accountId: account.accountId, name: account.name, enabled: account.enabled, configured: account.configured,
    devices: account.devices.size, voiceProvider: account.voice.provider, apiKeySource: account.voice.apiKeySource };
}

export function withSection(cfg, section) {
  return { ...cfg, channels: { ...cfg?.channels, [CHANNEL_ID]: section } };
}
