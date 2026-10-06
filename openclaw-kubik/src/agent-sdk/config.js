import { DEFAULT_VOICE } from '../config.js';
import { join, isAbsolute, resolve } from 'node:path';
import { ensurePrivateDirectory, readJsonFile, withFileLock, writeJsonAtomic } from './storage.js';
import { normalizeSetupValues, validateAdapter } from './adapter.js';

export const HOST_CONFIG_FILE = 'agent-host.json';
export const VOICE_SETUP_FIELDS = Object.freeze([
  ['baseUrl', 'Speech API URL'], ['transcribeModel', 'Recognition model'],
  ['ttsModel', 'Speech model'], ['voice', 'Speech voice'], ['language', 'Speech language (two-letter code, e.g. en or ru)'],
].map(([key, label]) => Object.freeze({ key, label, type: 'string', required: true, default: DEFAULT_VOICE[key] })));
const MAX_CONFIG_BYTES = 64 * 1024;

function hasUnknownKeys(value, allowed) { return Object.keys(value).some((key) => !allowed.includes(key)); }

function validateHostConfigShape(input) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new Error('agent host configuration has an unsupported format');
  if (input.version !== 2 || hasUnknownKeys(input, ['version', 'listener', 'voice', 'adapter'])) {
    throw new Error('agent host configuration has an unsupported format');
  }
}

function normalizeListener(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('agent host listener is invalid');
  if (hasUnknownKeys(value, ['port', 'host'])) throw new Error('agent host listener has an unknown field');
  if (!Number.isInteger(value.port)) throw new Error('agent host listener port must be an integer');
  if (value.port < 1 || value.port > 65535) throw new Error('agent host listener must declare a TCP port from 1 to 65535');
  if (value.host !== undefined && typeof value.host !== 'string') throw new Error('agent host listener host is invalid');
  const host = normalizeBindHost(value.host);
  return { port: value.port, ...(host ? { host } : {}) };
}

function normalizeBindHost(value) {
  if (value === undefined || value === '') return '';
  if (value.length > 255) throw new Error('agent host listener host is invalid');
  if (/[\u0000-\u001f]/u.test(value)) throw new Error('agent host listener host is invalid');
  return value;
}

function normalizeVoiceString(entry, key) {
  const limit = key === 'baseUrl' ? 2048 : 128;
  if (typeof entry !== 'string' || !entry.trim() || entry.length > limit || /[\u0000-\u001f\u007f]/u.test(entry)) {
    throw new Error(`agent host voice field "${key}" is invalid`);
  }
  return entry.trim();
}

function normalizeVoice(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('agent host voice is invalid');
  if (hasUnknownKeys(value, VOICE_SETUP_FIELDS.map((field) => field.key))) throw new Error('agent host voice has an unknown field');
  const voice = {};
  for (const field of VOICE_SETUP_FIELDS) {
    const entry = value[field.key] ?? field.default;
    voice[field.key] = normalizeVoiceString(entry, field.key);
  }
  if (!/^[a-z]{2}$/.test(voice.language)) throw new Error('speech language must be a lowercase two-letter code');
  return voice;
}

function validateAdapterConfigShape(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('agent adapter configuration is invalid');
  if (hasUnknownKeys(value, ['id', 'modulePath', 'setup'])) throw new Error('agent adapter configuration has an unknown field');
  if (typeof value.id !== 'string') throw new Error('agent adapter id is invalid');
  if (value.id.length > 40) throw new Error('agent adapter id is invalid');
  if (value.modulePath !== undefined) validateModulePath(value.modulePath);
  if (!value.setup || typeof value.setup !== 'object' || Array.isArray(value.setup)) throw new Error('agent adapter setup values are invalid');
}

function validateModulePath(value) {
  if (typeof value !== 'string' || !isAbsolute(value)) throw new Error('custom adapter modulePath must be absolute');
}

function normalizeAdapterConfig(value) {
  validateAdapterConfigShape(value);
  return { id: value.id, ...(value.modulePath ? { modulePath: resolve(value.modulePath) } : {}), setup: structuredClone(value.setup) };
}

export function normalizeHostConfig(input) {
  validateHostConfigShape(input);
  return {
    version: 2,
    listener: normalizeListener(input.listener),
    voice: normalizeVoice(input.voice),
    adapter: normalizeAdapterConfig(input.adapter),
  };
}

export function validateConfigForAdapter(config, adapter) {
  validateAdapter(adapter);
  if (config.adapter.id !== adapter.id) throw new Error(`configured adapter "${config.adapter.id}" does not match loaded adapter "${adapter.id}"`);
  return { ...config, adapter: { ...config.adapter, setup: normalizeSetupValues(adapter.setup, config.adapter.setup) } };
}

export function hostConfigPath(stateDir) { return join(stateDir, HOST_CONFIG_FILE); }

export async function writeHostConfig(stateDir, input) {
  ensurePrivateDirectory(stateDir);
  const config = normalizeHostConfig(input);
  const path = hostConfigPath(stateDir);
  await withFileLock(path, async () => writeJsonAtomic(path, config, MAX_CONFIG_BYTES));
  return config;
}

export async function readHostConfig(stateDir) {
  ensurePrivateDirectory(stateDir);
  const path = hostConfigPath(stateDir);
  return withFileLock(path, () => {
    const input = readJsonFile(path, MAX_CONFIG_BYTES);
    if (input?.version !== 1) return normalizeHostConfig(input);
    if (hasUnknownKeys(input, ['version', 'listener', 'voiceBaseUrl', 'adapter']) || typeof input.voiceBaseUrl !== 'string') {
      throw new Error('legacy agent host configuration has an unsupported format');
    }
    const config = normalizeHostConfig({ version: 2, listener: input.listener,
      voice: { baseUrl: input.voiceBaseUrl }, adapter: input.adapter });
    writeJsonAtomic(path, config, MAX_CONFIG_BYTES);
    return config;
  });
}
