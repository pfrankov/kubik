#!/usr/bin/env node
// Local mock of the OpenAI-compatible APIs Kubik needs, so everything runs without an OpenAI key.
import { randomUUID } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { fileURLToPath } from 'node:url';
import { BYTES_PER_MS, createStt, createSynth, pcmFromUpload, rms, STT_PHRASES, wav } from './audio.mjs';
import { lastUserUtterance, mockLlmReply } from './llm.mjs';
import { apiError, json, parseMultipart, readBody, sleep } from './http-utils.mjs';

export { BYTES_PER_MS, createStt, createSynth, pcmFromUpload, rms, STT_PHRASES, wav } from './audio.mjs';
export { lastUserUtterance, mockLlmReply } from './llm.mjs';

export async function startMockServer({ port = 18800, host = '127.0.0.1', tts = 'say', log = console.log, llmChunkDelayMs = 35,
  audioChunkDelayMs = 15, sttStart = 0 } = {}) {
  const synthesize = createSynth({ mode: tts, log });
  const transcribe = createStt({ start: sttStart });
  const stats = { chat: 0, transcriptions: 0, speech: 0, lastUserMessages: [] };

  async function handleChat(req, res) {
    const body = JSON.parse((await readBody(req)).toString('utf8') || '{}');
    const reply = mockLlmReply(body.messages);
    if (process.env.MOCK_DUMP_CHAT) await writeFile(process.env.MOCK_DUMP_CHAT, JSON.stringify(body, null, 2)).catch(() => {});
    stats.chat++;
    const lastUser = lastUserUtterance(body.messages);
    stats.lastUserMessages.push(lastUser.slice(-200));
    if (stats.lastUserMessages.length > 20) stats.lastUserMessages.shift();
    log(`mock llm: ${body.stream ? 'stream' : 'json'} model=${body.model} tools=${body.tools?.length ?? 0} user=${JSON.stringify(lastUser.slice(-80))} -> ${JSON.stringify(reply.slice(0, 60))}...`);
    const id = `chatcmpl-${randomUUID()}`;
    const created = Math.floor(Date.now() / 1000);
    const usage = { prompt_tokens: Math.ceil(JSON.stringify(body.messages ?? []).length / 4), completion_tokens: Math.ceil(reply.length / 4) };
    usage.total_tokens = usage.prompt_tokens + usage.completion_tokens;
    if (!body.stream) {
      return json(res, 200, { id, object: 'chat.completion', created, model: body.model,
        choices: [{ index: 0, message: { role: 'assistant', content: reply }, finish_reason: 'stop' }], usage });
    }
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' });
    const chunk = (delta, finish = null, extra = {}) => res.write(`data: ${JSON.stringify({ id, object: 'chat.completion.chunk', created, model: body.model,
      choices: [{ index: 0, delta, finish_reason: finish }], ...extra })}\n\n`);
    chunk({ role: 'assistant', content: '' });
    const pieces = reply.match(/\S+\s*|\s+/g) ?? [reply];
    for (let i = 0; i < pieces.length; i += 2) {
      if (res.destroyed) return;
      chunk({ content: pieces.slice(i, i + 2).join('') });
      await sleep(llmChunkDelayMs);
    }
    chunk({}, 'stop');
    if (body.stream_options?.include_usage) res.write(`data: ${JSON.stringify({ id, object: 'chat.completion.chunk', created, model: body.model, choices: [], usage })}\n\n`);
    res.end('data: [DONE]\n\n');
  }

  async function handleTranscription(req, res) {
    const parts = parseMultipart(await readBody(req), req.headers['content-type']);
    if (!parts?.file || !Buffer.isBuffer(parts.file)) return apiError(res, 400, 'file is required');
    const pcm = pcmFromUpload(parts.file);
    const text = transcribe(pcm);
    stats.transcriptions++;
    log(`mock stt(http): ${(pcm.length / BYTES_PER_MS).toFixed(0)} ms rms=${rms(pcm).toFixed(3)} -> ${JSON.stringify(text)}`);
    json(res, 200, parts.response_format === 'text' ? text : { text });
  }

  async function handleSpeech(req, res) {
    const body = JSON.parse((await readBody(req)).toString('utf8') || '{}');
    if (typeof body.input !== 'string' || !body.input.trim()) return apiError(res, 400, 'input is required');
    const format = body.response_format ?? 'mp3';
    if (!['pcm', 'wav'].includes(format)) return apiError(res, 400, `mock supports response_format pcm or wav, not ${format}`);
    stats.speech++;
    const pcm = await synthesize(body.input);
    log(`mock tts(http): ${JSON.stringify(body.input.slice(0, 60))} -> ${(pcm.length / BYTES_PER_MS).toFixed(0)} ms ${format}`);
    if (format === 'wav') { res.writeHead(200, { 'Content-Type': 'audio/wav' }); return res.end(wav(pcm)); }
    res.writeHead(200, { 'Content-Type': 'audio/pcm' });
    for (let off = 0; off < pcm.length && !res.destroyed; off += 9600) { res.write(pcm.subarray(off, off + 9600)); await sleep(audioChunkDelayMs); }
    res.end();
  }

  const models = (_req, res) => json(res, 200, { object: 'list', data: ['gpt-4o-transcribe', 'gpt-4o-mini-tts', 'mock-llm']
    .map((id) => ({ id, object: 'model', created: 0, owned_by: 'mock' })) });
  const routes = new Map([
    ['GET /v1/models', models],
    ['GET /models', models],
    ['GET /health', (_req, res) => json(res, 200, { ok: true, stats })],
    ['POST /v1/chat/completions', handleChat],
    ['POST /v1/audio/transcriptions', handleTranscription],
    ['POST /v1/audio/speech', handleSpeech],
  ]);
  const http = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://mock');
    try {
      const handler = routes.get(`${req.method} ${url.pathname}`);
      if (!handler) return apiError(res, 404, `mock: no route ${req.method} ${url.pathname}`);
      return await handler(req, res);
    } catch (error) {
      log(`mock: ${req.method} ${url.pathname} failed: ${error.message}`);
      if (!res.headersSent) apiError(res, error.status ?? 500, error.message); else res.destroy();
    }
  });

  await new Promise((resolve, reject) => { http.once('error', reject); http.listen(port, host, resolve); });
  const address = http.address();
  return {
    port: address.port, url: `http://${host}:${address.port}/v1`, stats,
    close: async () => {
      http.closeAllConnections();
      await new Promise((r) => http.close(() => r()));
    },
  };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const arg = (name, fallback) => { const i = process.argv.indexOf(`--${name}`); return i > 0 ? process.argv[i + 1] : fallback; };
  const ts = () => new Date().toISOString().slice(11, 23);
  const server = await startMockServer({ port: Number(arg('port', process.env.MOCK_OPENAI_PORT ?? 18800)), host: arg('host', '127.0.0.1'),
    tts: arg('tts', 'say'), sttStart: Number(arg('stt-start', 0)), log: (m) => console.log(`${ts()} ${m}`) });
  console.log(`${ts()} mock-openai listening on ${server.url} (tts=${arg('tts', 'say')})`);
  const stop = async () => { await server.close(); process.exit(0); };
  process.on('SIGINT', stop); process.on('SIGTERM', stop);
}
