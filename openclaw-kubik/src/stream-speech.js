import { StreamPitch } from './stream-pitch.js';
import { VoiceError } from './engines/common.js';

export async function speechSdk() {
  const [tts, http, speech] = await Promise.all([
    import('openclaw/plugin-sdk/tts-runtime'), import('openclaw/plugin-sdk/provider-http'),
    import('openclaw/plugin-sdk/speech'),
  ]);
  return { ...tts, ...http, ...speech };
}

async function openAiStream(sdk, cfg, providerConfig, text, timeoutMs, signal) {
  const apiKey = providerConfig.apiKey || process.env.OPENAI_API_KEY;
  if (!apiKey) throw new VoiceError('OpenAI TTS has no configured API key');
  const baseUrl = providerConfig.baseUrl || 'https://api.openai.com/v1';
  const request = cfg.models?.providers?.openai?.request;
  const policy = sdk.resolveProviderHttpRequestConfig({ baseUrl, defaultBaseUrl: 'https://api.openai.com/v1',
    allowPrivateNetwork: request?.allowPrivateNetwork === true,
    defaultHeaders: { Authorization: `Bearer ${apiKey}`, 'Content-Type': 'application/json' },
    provider: 'openai', capability: 'audio', transport: 'http', request });
  const resource = await sdk.postJsonRequest({ url: `${baseUrl.replace(/\/+$/, '')}/audio/speech`, ...policy,
    timeoutMs, signal, fetchFn: fetch, body: { ...providerConfig.extraBody,
      model: providerConfig.model, voice: providerConfig.voice, input: text, response_format: 'pcm',
      ...(providerConfig.instructions ? { instructions: providerConfig.instructions } : {}),
      ...(providerConfig.speed ? { speed: providerConfig.speed } : {}),
    } });
  if (!resource.response.ok) {
    await resource.release(); throw new VoiceError(`speech failed (HTTP ${resource.response.status})`);
  }
  return { success: true, audioStream: resource.response.body, outputFormat: 'pcm', release: resource.release };
}

/** Same configured provider, PCM only, no provider retries/fallback. Releases on completion, cancel and error. */
export async function streamDeviceSpeech({ cfg, text, pitch, signal, timeoutMs, onAudio, normalizePcm, selection, loadSdk = speechSdk }) {
  const sdk = await loadSdk();
  const config = sdk.resolveTtsConfig(cfg);
  const provider = sdk.getTtsProvider(config, sdk.resolveTtsPrefsPath(config));
  let providerConfig = sdk.getResolvedSpeechProviderConfig(config, provider, cfg);
  if (selection && selection.provider !== provider) throw new VoiceError('Selected speech provider is unavailable');
  const overrides = selection ? sdk.resolveExplicitTtsOverrides({ cfg, provider, modelId: selection.model }) : { provider };
  const providerOverrides = overrides.providerOverrides?.[provider] ?? {};
  providerConfig = { ...providerConfig, ...providerOverrides };
  const combined = AbortSignal.any([signal, AbortSignal.timeout(timeoutMs)]);
  combined.throwIfAborted();
  if (!['openai', 'elevenlabs'].includes(provider)) {
    const implementation = sdk.getSpeechProvider(provider, cfg);
    if (!implementation?.synthesizeTelephony) throw new VoiceError('configured TTS provider has no PCM output');
    const result = await abortable(implementation.synthesizeTelephony({ text, cfg, providerConfig, timeoutMs }), combined);
    combined.throwIfAborted();
    const transform = new StreamPitch(pitch);
    const pcm = normalizePcm(result);
    if (!pcm.length) throw new VoiceError('speech synthesis returned no audio');
    if (pcm.length > 48_000 * 60) throw new VoiceError('speech synthesis exceeds one minute');
    onAudio?.(transform.push(pcm)); onAudio?.(transform.finish());
    return;
  }
  let stream;
  try {
    stream = provider === 'openai'
      ? await openAiStream(sdk, cfg, providerConfig, text, timeoutMs, combined)
      : await acquireStream(sdk.streamSpeech({ text, cfg, timeoutMs, channel: 'kubik', disableFallback: true,
        overrides: { provider, providerOverrides: { [provider]: { ...providerOverrides, outputFormat: 'pcm_24000' } } } }), combined);
    combined.throwIfAborted();
    if (!stream.success || !stream.audioStream) throw new VoiceError('configured provider does not support streaming speech');
    if (!/^pcm(?:_24000)?$/i.test(stream.outputFormat ?? '')) throw new VoiceError('streaming speech must return 24 kHz PCM');
    const transform = new StreamPitch(pitch);
    const cancel = () => { stream.release?.().catch(() => {}); };
    combined.addEventListener('abort', cancel, { once: true });
    let bytes = 0;
    try {
      for await (const chunk of stream.audioStream) {
        combined.throwIfAborted(); bytes += chunk.length;
        if (bytes > 48_000 * 60) throw new VoiceError('streaming speech exceeds one minute');
        const pcm = transform.push(Buffer.from(chunk));
        if (pcm.length) onAudio?.(pcm);
      }
      combined.throwIfAborted();
      const tail = transform.finish();
      if (tail.length) onAudio?.(tail);
      if (!bytes) throw new VoiceError('speech synthesis returned no audio');
    } finally { combined.removeEventListener('abort', cancel); }
  } finally { await stream?.release?.(); }
}

function abortable(promise, signal) {
  if (signal.aborted) { promise.catch(() => {}); return Promise.reject(signal.reason); }
  return new Promise((resolve, reject) => {
    const onAbort = () => { cleanup(); reject(signal.reason); };
    const cleanup = () => signal.removeEventListener('abort', onAbort);
    signal.addEventListener('abort', onAbort, { once: true });
    promise.then((value) => { cleanup(); resolve(value); }, (error) => { cleanup(); reject(error); });
  });
}

async function acquireStream(promise, signal) {
  promise.then((stream) => { if (signal.aborted) stream.release?.().catch(() => {}); }, () => {});
  return abortable(promise, signal);
}
