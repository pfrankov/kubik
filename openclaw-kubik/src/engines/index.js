import { HttpEngine } from './http.js';
import { OpenClawEngine } from './openclaw.js';
export { HttpEngine, OpenClawEngine };
export { VoiceError } from './common.js';

/**
 * Engine contract (all providers):
 *   beginTurn()                         reset the input buffer for a new utterance
 *   append(pcm)                         add 24 kHz s16le mono microphone audio
 *   commit({ signal }) -> transcript    exact transcript of the utterance ('' when nothing was said)
 *   speak(text, { onAudio, signal })    stream speech PCM; resolves { cancelled } when generation ends
 *   cancel() / cancelTranscription()    stop the current generation / transcription wait
 *   close()
 */
export function createEngine(voice, options = {}) {
  if (voice.provider === 'openai-http') return new HttpEngine(voice, options);
  return new OpenClawEngine(voice, options);
}
