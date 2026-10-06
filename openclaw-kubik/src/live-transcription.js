import WebSocket from 'ws';
import { VoiceError } from './engines/common.js';

export const LIVE_TRANSCRIPTION_MODEL = 'gpt-live-transcribe';
const MAX_BYTES = 48_000 * 60;
const MAX_PENDING = 1024 * 1024;
const fail = (message) => new VoiceError(message, { code: 'stt_failed' });
const providerFailed = (event) => ['error', 'conversation.item.input_audio_transcription.failed'].includes(event.type);

export async function resolveLiveCredential({ cfg, signal, agentDir, env = process.env, resolveAuth } = {}) {
  const platformKey = env.OPENAI_API_KEY?.trim();
  if (platformKey) return platformKey;
  const auth = await resolveAuth({ provider: 'openai', cfg, signal, ...(agentDir ? { agentDir } : {}) });
  if (!auth?.apiKey || auth.mode !== 'api-key') throw fail('Live transcription requires a configured OpenAI API key');
  return auth.apiKey;
}

export async function openAiCredential(cfg, signal, agentDir) {
  const { resolveApiKeyForProvider } = await import('openclaw/plugin-sdk/provider-auth-runtime');
  return resolveLiveCredential({ cfg, signal, agentDir, resolveAuth: resolveApiKeyForProvider });
}

/** One PTT turn, one transcription-only socket. No VAD, ambient input, assistant or batch fallback. */
export class LiveTranscription {
  constructor({ cfg, language = 'ru', timeoutMs = 45_000, connectTimeoutMs = 10_000,
    credential = openAiCredential, Socket = WebSocket } = {}) {
    Object.assign(this, { cfg, language, timeoutMs, connectTimeoutMs, credential, Socket });
    this.controller = new AbortController(); this.queue = []; this.pending = 0; this.bytes = 0;
    this.ready = false; this.committing = false; this.item = null; this.result = null;
    this.finished = false;
    this.promise = new Promise((resolve, reject) => { this.resolve = resolve; this.reject = reject; });
    this.promise.catch(() => {}); // failure may precede PTT release; commit still observes it.
    this.connect().catch((error) => this.finish(error));
  }
  async connect() {
    this.connectTimer = setTimeout(() => this.finish(fail('Live transcription connection timed out')), this.connectTimeoutMs);
    const key = await this.credential(this.cfg, this.controller.signal);
    if (this.finished) return;
    this.socket = new this.Socket('wss://api.openai.com/v1/realtime?intent=transcription', {
      headers: { Authorization: `Bearer ${key}` }, maxPayload: MAX_PENDING,
    });
    this.socket.on('open', () => this.send({ type: 'session.update', session: {
      type: 'transcription', audio: { input: { format: { type: 'audio/pcm', rate: 24000 },
        transcription: { model: LIVE_TRANSCRIPTION_MODEL, ...(this.language ? { languages: [this.language] } : {}), delay: 'low' },
        turn_detection: null } },
    } }));
    this.socket.on('message', (data) => this.message(data));
    this.socket.on('error', () => this.finish(fail('Live transcription connection failed')));
    this.socket.on('close', () => { if (!this.finished) this.finish(fail('Live transcription connection closed')); });
    this.turnTimer = setTimeout(() => this.finish(fail('Live transcription recording exceeds one minute')), 60_000);
  }
  send(value) {
    if (this.finished || !this.socket || this.socket.readyState !== WebSocket.OPEN) return;
    if (this.socket.bufferedAmount > MAX_PENDING) { this.finish(fail('Live transcription connection is too slow')); return; }
    this.socket.send(JSON.stringify(value));
  }
  append(pcm) {
    if (this.finished || this.committing || !pcm?.length) return;
    if (pcm.length % 2) { this.finish(fail('Live transcription expects complete PCM samples')); return; }
    this.bytes += pcm.length;
    if (this.bytes > MAX_BYTES) { this.finish(fail('Live transcription recording exceeds one minute')); return; }
    if (this.ready) this.send({ type: 'input_audio_buffer.append', audio: Buffer.from(pcm).toString('base64') });
    else {
      this.pending += pcm.length;
      if (this.pending > MAX_PENDING) { this.finish(fail('Live transcription startup buffer is full')); return; }
      this.queue.push(Buffer.from(pcm));
    }
  }
  message(data) {
    if (this.finished) return;
    let event;
    try { event = JSON.parse(String(data)); } catch { this.finish(fail('Invalid live transcription event')); return; }
    if (providerFailed(event)) {
      this.finish(fail('Live transcription failed; check OpenAI model access and credentials')); return;
    }
    if (event.type === 'session.updated') {
      clearTimeout(this.connectTimer); this.ready = true;
      for (const pcm of this.queue) this.send({ type: 'input_audio_buffer.append', audio: pcm.toString('base64') });
      this.queue = []; this.pending = 0;
      if (this.committing) this.sendCommit();
    }
    if (event.type === 'input_audio_buffer.committed') this.item = event.item_id;
    if (event.type === 'conversation.item.input_audio_transcription.completed') this.result = event;
    this.complete();
  }
  complete() {
    if (this.committing && this.item && this.result?.item_id === this.item) this.finish(null, String(this.result.transcript ?? '').trim());
  }
  sendCommit() {
    if (this.finished || this.commitSent) return;
    this.commitSent = true; this.send({ type: 'input_audio_buffer.commit' });
  }
  commit({ signal } = {}) {
    if (this.finished) return this.promise;
    if (!this.committing) {
      this.committing = true; clearTimeout(this.turnTimer);
      if (this.bytes < 4800) this.finish(null, '');
      else {
        this.resultTimer = setTimeout(() => this.finish(fail('Live transcription result timed out')), this.timeoutMs);
        if (this.ready) this.sendCommit();
      }
    }
    const abort = () => this.cancel();
    if (signal?.aborted) abort(); else signal?.addEventListener('abort', abort, { once: true });
    return this.promise.finally(() => signal?.removeEventListener('abort', abort));
  }
  cancel() { const error = new Error('aborted'); error.name = 'AbortError'; this.finish(error); }
  finish(error, text) {
    if (this.finished) return;
    this.finished = true; this.controller.abort();
    for (const timer of [this.connectTimer, this.turnTimer, this.resultTimer]) clearTimeout(timer);
    this.queue = []; this.pending = 0;
    this.socket?.terminate();
    if (error) this.reject(error instanceof VoiceError || error?.name === 'AbortError' ? error : fail('Live transcription initialization failed'));
    else this.resolve(text);
  }
}
