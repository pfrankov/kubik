import { readJsonBounded } from '../http-response.js';
import { pcmToWav, SampleAligner } from '../audio.js';
import { BYTES_PER_MS } from '../protocol.js';
import { authHeaders, DEFAULT_VOICE_STYLE, timeoutSignal, VoiceError } from './common.js';

const MAX_TURN_BYTES = 60_000 * BYTES_PER_MS; // one minute of speech

/** Classic OpenAI HTTP audio endpoints: /audio/transcriptions + streamed /audio/speech (pcm). */
export class HttpEngine {
  #chunks = [];
  #bytes = 0;
  #speech = null;
  #commit = null;
  constructor(voice, { log = () => {}, fetchImpl = fetch, transcribeTimeoutMs = 30_000, speechTimeoutMs = 60_000 } = {}) {
    this.voice = voice;
    Object.assign(this, { log, fetchImpl, transcribeTimeoutMs, speechTimeoutMs });
  }
  get connected() { return true; }
  get canListen() { return Boolean(this.voice.apiKey && this.voice.transcribeModel); }
  get canSpeak() { return Boolean(this.voice.apiKey && this.voice.ttsModel && this.voice.voice); }
  beginTurn() { this.#chunks = []; this.#bytes = 0; this.#commit?.abort(); }
  append(pcm) {
    if (!pcm?.length || this.#bytes + pcm.length > MAX_TURN_BYTES) return;
    this.#chunks.push(Buffer.from(pcm)); this.#bytes += pcm.length;
  }
  async commit({ signal } = {}) {
    const pcm = Buffer.concat(this.#chunks, this.#bytes);
    this.#chunks = []; this.#bytes = 0;
    if (pcm.length / BYTES_PER_MS < 100) return '';
    const controller = new AbortController();
    this.#commit = controller;
    const form = new FormData();
    form.set('file', new Blob([pcmToWav(pcm)], { type: 'audio/wav' }), 'turn.wav');
    form.set('model', this.voice.transcribeModel);
    if (this.voice.language) form.set('language', this.voice.language);
    form.set('response_format', 'json');
    try {
      let response;
      try {
        response = await this.fetchImpl(`${this.voice.baseUrl}/audio/transcriptions`, {
          method: 'POST', headers: authHeaders(this.voice), body: form,
          signal: timeoutSignal(this.transcribeTimeoutMs, signal ? AbortSignal.any([signal, controller.signal]) : controller.signal),
        });
      } catch (error) {
        if (controller.signal.aborted || signal?.aborted) throw error;
        throw new VoiceError('transcription request failed', { code: 'stt_failed' });
      }
      if (!response.ok) { await response.body?.cancel(); throw new VoiceError(`transcription failed (HTTP ${response.status})`, { code: 'stt_failed', status: response.status }); }
      let body;
      try { body = await readJsonBounded(response, 64 * 1024, 'transcription'); }
      catch (error) {
        if (controller.signal.aborted || signal?.aborted) throw error;
        throw new VoiceError('transcription response is invalid or exceeds 64 KiB', { code: 'stt_failed' });
      }
      if (typeof body?.text !== 'string') return '';
      const text = body.text.trim();
      if (text.length > 8192) throw new VoiceError('transcription exceeds 8192 characters', { code: 'stt_failed' });
      return text;
    } finally { if (this.#commit === controller) this.#commit = null; }
  }
  async speak(text, { onAudio, signal } = {}) {
    const controller = new AbortController();
    this.#speech = controller;
    const combined = signal ? AbortSignal.any([signal, controller.signal]) : controller.signal;
    const aligner = new SampleAligner();
    try {
      const response = await this.fetchImpl(`${this.voice.baseUrl}/audio/speech`, {
        method: 'POST', headers: { ...authHeaders(this.voice), 'Content-Type': 'application/json' },
        body: JSON.stringify({ model: this.voice.ttsModel, voice: this.voice.voice, input: text, response_format: 'pcm',
          instructions: this.voice.speechInstructions?.trim() || DEFAULT_VOICE_STYLE }),
        signal: timeoutSignal(this.speechTimeoutMs, combined),
      });
      if (!response.ok) { await response.body?.cancel(); throw new VoiceError(`speech failed (HTTP ${response.status})`, { status: response.status }); }
      for await (const chunk of response.body) {
        if (combined.aborted) break;
        const pcm = aligner.push(Buffer.from(chunk));
        if (pcm.length) onAudio?.(pcm);
      }
      return { cancelled: combined.aborted };
    } catch (error) {
      if (combined.aborted) return { cancelled: true };
      if (error instanceof VoiceError) throw error;
      throw new VoiceError('speech request failed');
    } finally { if (this.#speech === controller) this.#speech = null; }
  }
  cancel() { this.#speech?.abort(); }
  cancelTranscription() { this.#chunks = []; this.#bytes = 0; this.#commit?.abort(); }
  close() { this.#speech?.abort(); this.#commit?.abort(); }
}
