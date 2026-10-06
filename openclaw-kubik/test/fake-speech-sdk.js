import { toDevicePcm } from '../src/engines/openclaw.js';

/** Provider transport stand-in. Uses the recorded fake core result, never imports or invokes a paid provider. */
export function fakeSpeechSdk(core) {
  let cfg;
  const synthesize = async (text, signal) => {
    const result = await abortable(core.tts.textToSpeechTelephony({ text, cfg }), signal);
    if (!result.success) return { success: false, release: async () => {} };
    const pcm = toDevicePcm(result);
    async function* audioStream() { for (let i = 0; i < pcm.length; i += 9600) yield pcm.subarray(i, i + 9600); }
    return { success: true, outputFormat: 'pcm', audioStream: audioStream(), release: async () => {} };
  };
  return async () => ({ resolveTtsConfig: (value) => { cfg = value; return value; },
    resolveTtsPrefsPath: () => '/unused', getTtsProvider: () => cfg.tts?.provider ?? 'openai',
    getResolvedSpeechProviderConfig: () => ({ apiKey: 'test-key', model: 'test-model', voice: 'test-voice' }),
    resolveProviderHttpRequestConfig: (params) => ({ headers: params.defaultHeaders }),
    postJsonRequest: async (params) => {
      const result = await synthesize(params.body.input, params.signal);
      return { response: { ok: result.success, status: result.success ? 200 : 500, body: result.audioStream }, release: result.release };
    },
    streamSpeech: (params) => synthesize(params.text),
  });
}
function abortable(promise, signal) {
  if (!signal) return promise;
  return new Promise((resolve, reject) => {
    const abort = () => reject(signal.reason);
    signal.addEventListener('abort', abort, { once: true });
    promise.then(resolve, reject).finally(() => signal.removeEventListener('abort', abort));
  });
}

export const speechConfig = (cfg = {}) => ({ ...cfg,
  tts: { provider: 'openai', providers: { openai: { apiKey: 'test-key' }, elevenlabs: { apiKey: 'test-key' } }, ...cfg.tts } });
