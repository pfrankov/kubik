// Provider-neutral model hooks. Speech belongs to this host, not to the agent adapter.
const fail = code => Object.assign(new Error(code), {code});
function text(value, max) {
  return typeof value === 'string' && value.trim() && Buffer.byteLength(value) <= max && !/[\p{C}]/u.test(value);
}
function catalog(value) {
  if (!value || !Array.isArray(value.models) || value.models.length > 128 ||
      (value.model !== '' && !text(value.model, 160))) throw fail('unavailable');
  const ids = new Set();
  const models = value.models.map(row => {
    if (!text(row?.id, 160) || !text(row?.label, 80) || ids.has(row.id)) throw fail('unavailable');
    ids.add(row.id); return {id: row.id, label: row.label};
  });
  if (value.model && !ids.has(value.model)) throw fail('unavailable');
  return {model: value.model, models};
}

export function createAdapterControl(adapter, voice) {
  const available = Boolean(voice.apiKey);
  const speech = {
    stt: {available, provider: 'Host', model: voice.transcribeModel},
    tts: {available, provider: 'Host', model: voice.ttsModel},
  };
  async function models(device) {
    return adapter.modelOptions ? catalog(await adapter.modelOptions(device)) : {model: '', models: []};
  }
  return {
    async options(device, target = 'agent') {
      if (target === 'mode') return { model: 'classic', models: [
        { id: 'classic', label: 'STT', available },
        { id: 'realtime', label: 'Realtime', available: false },
        { id: 'live', label: 'GPT Live', available: false },
      ], ...speech };
      if (target !== 'agent') return { model: '', models: [], ...speech };
      return {...await models(device), ...speech};
    },
    async selectModel(device, id, target = 'agent') {
      if (target !== 'agent') throw fail('unsupported');
      if (!adapter.selectModel) throw fail('unsupported');
      if (!(await models(device)).models.some(row => row.id === id)) throw fail('invalid_model');
      await adapter.selectModel(device, id);
      if ((await models(device)).model !== id) throw fail('unavailable');
    },
  };
}
