import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const ADAPTER_ID = /^[a-z][a-z0-9-]{0,39}$/;
const SETUP_KEY = /^[a-z][A-Za-z0-9]{0,39}$/;
const ENV_NAME = /^[A-Za-z_][A-Za-z0-9_]{0,63}$/;
const FIELD_TYPES = new Set(['string', 'integer', 'boolean', 'env']);
const SENSITIVE = /(api.?key|token|secret|password|credential|bearer)/i;
const hasOwn = (object, key) => Object.prototype.hasOwnProperty.call(object, key);

function validateFieldProperties(field) {
  const allowed = new Set(['key', 'label', 'type', 'required', 'default',
    ...(field.type === 'string' ? ['maxLength'] : []),
    ...(field.type === 'integer' ? ['min', 'max'] : [])]);
  for (const key of Object.keys(field)) {
    if (!allowed.has(key)) throw new Error(`adapter setup field "${field.key}" has an unknown property "${key}"`);
  }
  if (field.required !== undefined && typeof field.required !== 'boolean') {
    throw new Error(`adapter setup field "${field.key}" required must be true or false`);
  }
}

function validateFieldShape(field) {
  if (!field || typeof field !== 'object' || Array.isArray(field)) throw new Error('adapter has an invalid setup field');
  if (!SETUP_KEY.test(field.key)) throw new Error('adapter setup field has an invalid key');
  if (typeof field.label !== 'string' || !field.label.trim() || field.label.length > 80) throw new Error('adapter setup field has an invalid label');
  if (!FIELD_TYPES.has(field.type)) throw new Error(`adapter setup field "${field.key}" has an unsupported type`);
  validateFieldProperties(field);
}

function validateSensitiveField(field) {
  if (SENSITIVE.test(`${field.key} ${field.label}`) && field.type !== 'env') {
    throw new Error(`sensitive setup field "${field.key}" must store an environment-variable name, not a secret`);
  }
}

function validateStringBounds(field) {
  const maxLength = field.maxLength === undefined ? 1024 : field.maxLength;
  if (!Number.isSafeInteger(maxLength) || maxLength < 1 || maxLength > 2048) {
    throw new Error(`adapter setup field "${field.key}" has an invalid maxLength`);
  }
}

function validateIntegerBounds(field) {
  if (field.min !== undefined && !Number.isSafeInteger(field.min)) throw new Error(`adapter setup field "${field.key}" has invalid integer bounds`);
  if (field.max !== undefined && !Number.isSafeInteger(field.max)) throw new Error(`adapter setup field "${field.key}" has invalid integer bounds`);
  if (field.min !== undefined && field.max !== undefined && field.min > field.max) {
    throw new Error(`adapter setup field "${field.key}" has invalid integer bounds`);
  }
}

function validateFieldBounds(field) {
  if (field.type === 'string') validateStringBounds(field);
  if (field.type === 'integer') validateIntegerBounds(field);
}

function validateField(field) {
  validateFieldShape(field);
  validateSensitiveField(field);
  validateFieldBounds(field);
  if (hasOwn(field, 'default')) validateFieldValue(field, field.default);
  const maxLength = field.maxLength === undefined ? 1024 : field.maxLength;
  return { ...field, ...(field.type === 'string' ? { maxLength } : {}) };
}

function validateStringValue(field, value) {
  const maxLength = field.maxLength === undefined ? 1024 : field.maxLength;
  if (typeof value !== 'string' || value.length > maxLength
    || /[\u0000-\u0008\u000b\u000c\u000e-\u001f]/u.test(value)) {
    throw new Error(`setup field "${field.key}" must be a string of at most ${maxLength} characters`);
  }
  if (field.required && !value.trim()) throw new Error(`setup field "${field.key}" is required`);
  return value;
}

function validateIntegerValue(field, value) {
  if (!Number.isSafeInteger(value)) throw new Error(`setup field "${field.key}" must be an integer`);
  if (field.min !== undefined && value < field.min) throw new Error(`setup field "${field.key}" must be >= ${field.min}`);
  if (field.max !== undefined && value > field.max) throw new Error(`setup field "${field.key}" must be <= ${field.max}`);
  return value;
}

function validateFieldValue(field, value) {
  if (field.type === 'string') return validateStringValue(field, value);
  if (field.type === 'integer') return validateIntegerValue(field, value);
  if (field.type === 'boolean') {
    if (typeof value !== 'boolean') throw new Error(`setup field "${field.key}" must be true or false`);
    return value;
  }
  if (typeof value !== 'string' || !ENV_NAME.test(value)) throw new Error(`setup field "${field.key}" must be an environment-variable name`);
  return value;
}

function validateAdapterMethods(adapter) {
  for (const hook of ['connect', 'dispatch']) {
    if (typeof adapter[hook] !== 'function') throw new Error(`agent adapter must implement ${hook}()`);
  }
  for (const hook of ['start', 'close']) {
    if (adapter[hook] !== undefined && typeof adapter[hook] !== 'function') throw new Error(`agent adapter ${hook} must be a function`);
  }
  if (adapter.modelOptions !== undefined || adapter.selectModel !== undefined) {
    if (typeof adapter.modelOptions !== 'function' || typeof adapter.selectModel !== 'function') {
      throw new Error('agent model controls require both modelOptions() and selectModel()');
    }
  }
}

function validateAdapterShape(adapter) {
  if (!adapter || typeof adapter !== 'object' || Array.isArray(adapter)) throw new Error('agent adapter must be an object');
  if (!ADAPTER_ID.test(adapter.id ?? '')) throw new Error('agent adapter id is invalid');
  if (typeof adapter.label !== 'string' || !adapter.label.trim() || adapter.label.length > 80) throw new Error('agent adapter label is invalid');
  if (!Array.isArray(adapter.setup) || adapter.setup.length > 32) throw new Error('agent adapter setup must be an array of at most 32 fields');
  validateAdapterMethods(adapter);
}

export function validateAdapter(adapter) {
  validateAdapterShape(adapter);
  const fields = adapter.setup.map(validateField);
  if (new Set(fields.map((field) => field.key)).size !== fields.length) throw new Error('adapter setup field keys must be unique');
  return adapter;
}

export function normalizeSetupValues(fields, input = {}) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new Error('adapter setup values must be an object');
  const checkedFields = fields.map(validateField);
  const known = new Set(checkedFields.map((field) => field.key));
  for (const key of Object.keys(input)) if (!known.has(key)) throw new Error(`unknown adapter setup field "${key}"`);
  const values = {};
  for (const field of checkedFields) {
    let value = hasOwn(input, field.key) ? input[field.key] : field.default;
    if (value === undefined) {
      if (field.required) throw new Error(`setup field "${field.key}" is required`);
      continue;
    }
    values[field.key] = validateFieldValue(field, value);
  }
  return values;
}

/** Resolves an `env` setup value at the process boundary; the variable name remains the only saved value. */
export function readSetupEnv(setup, key, env = process.env) {
  const name = setup?.[key];
  if (typeof name !== 'string' || !ENV_NAME.test(name)) throw new Error(`setup field "${key}" must name an environment variable`);
  const value = env[name]?.trim();
  if (!value) throw new Error(`Set ${name} in the environment before starting this adapter`);
  return value;
}

/** Load the default ESM export from a configured local module. The path is resolved by the user, never from input data. */
export async function importAdapter(modulePath) {
  if (typeof modulePath !== 'string' || !modulePath.trim()) throw new Error('custom adapter module path is required');
  const absolute = resolve(modulePath);
  const module = await import(pathToFileURL(absolute).href);
  return validateAdapter(module.default);
}
