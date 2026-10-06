export const DEFAULT_VOICE_STYLE = 'Voice: warm, gentle and friendly, a little playful, like a small cute desk companion. '
  + 'Natural conversational pace, clear pronunciation.';

export class VoiceError extends Error {
  constructor(message, { code = 'voice_failed', status } = {}) {
    super(message); this.name = 'VoiceError'; this.code = code; if (status) this.status = status;
  }
}

export const authHeaders = (voice) => (voice.apiKey ? { Authorization: `Bearer ${voice.apiKey}` } : {});

/** Combines an optional caller signal with a timeout; returns { signal, dispose }. */
export function timeoutSignal(ms, signal) {
  const timeout = AbortSignal.timeout(ms);
  return signal ? AbortSignal.any([signal, timeout]) : timeout;
}
