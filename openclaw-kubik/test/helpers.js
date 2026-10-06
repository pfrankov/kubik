import { createHash, generateKeyPairSync, sign } from 'node:crypto';
import { once } from 'node:events';
import { readFileSync } from 'node:fs';
import { parseWav } from '../src/audio.js';
import WebSocket from 'ws';
import { resolveAccount } from '../src/config.js';
import { spkiSha256, ImaEncoder, KIND_MIC } from '../src/protocol.js';
import { setRuntime } from '../src/runtime.js';

export const DEVICE = 'kubik-b6c634';

export const config = (settings = {}) => ({ channels: { kubik: {
  listen: { port: 18790 },
  devices: { [DEVICE]: { name: 'Desk' } },
  voice: { provider: 'openai-http', baseUrl: 'http://127.0.0.1:1/v1', apiKey: 'sk-test-DO-NOT-LOG' },
  allowInsecureBaseUrl: true,
  ...settings } } });
/** Resolved account bound to an ephemeral port (0 is not a valid config value). */
export const account = (settings = {}) => {
  const resolved = resolveAccount(config(settings), 'default', { env: {} });
  resolved.listen = { ...resolved.listen, port: 0 };
  return resolved;
};

/** Starts the mock OpenAI server from tools/ (tone TTS: fast, no `say`). */
export async function mockOpenAI(t, options = {}) {
  const { startMockServer } = await import('../../tools/mock-openai/server.mjs');
  const logs = [];
  const server = await startMockServer({ port: 0, tts: 'tone', log: (m) => logs.push(m), audioChunkDelayMs: 1, llmChunkDelayMs: 1, ...options });
  t.after(() => server.close());
  return { ...server, logs };
}

/** 24 kHz s16le sine "speech" of the given length. */
export function tone(ms, { freq = 220, amp = 8000 } = {}) {
  const samples = Math.round(ms * 24);
  const pcm = Buffer.alloc(samples * 2);
  for (let i = 0; i < samples; i++) pcm.writeInt16LE(Math.round(amp * Math.sin(2 * Math.PI * freq * i / 24000)), i * 2);
  return pcm;
}

/** Fake engine: deterministic transcripts, speech = 1 ms of audio per character. */
export function fakeEngine({ transcripts = ['Привет'], speakDelayMs = 5, failSpeak = false } = {}) {
  const calls = { begin: 0, append: 0, commit: 0, speak: [], cancel: 0, cancelTranscription: 0 };
  let next = 0;
  return {
    calls,
    beginTurn() { calls.begin++; },
    append() { calls.append++; },
    async commit() { calls.commit++; await sleep(5); return transcripts[next++ % transcripts.length]; },
    async speak(text, { onAudio, signal }) {
      calls.speak.push(text);
      if (failSpeak) throw new Error('boom');
      const pcm = tone(Math.max(20, text.length));
      for (let off = 0; off < pcm.length; off += 4800) {
        if (signal?.aborted) return { cancelled: true };
        await sleep(speakDelayMs);
        if (signal?.aborted) return { cancelled: true };
        onAudio(pcm.subarray(off, off + 4800));
      }
      return { cancelled: false };
    },
    cancel() { calls.cancel++; },
    cancelTranscription() { calls.cancelTranscription++; },
    close() {},
  };
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** The gateway-route binding a current device signs for `url`: `ca:` and the URL's host (lowercase, no port or IPv6 brackets). */
export const caBindFor = (url) => `ca:${new URL(url).hostname.replace(/^\[|\]$/g, '').replace(/\.$/, '')}`;

/** The binding of a device that connected to the fake Gateway (`fakeGateway` listens on 127.0.0.1). */
export const FAKE_GATEWAY_BIND = 'ca:127.0.0.1';

/** A v5 device identity: P-256 key pair, wire key (base64 SEC1 point) and allow-store entry id.
 * `sign(nonce, { bind })` signs the transport binding the device saw (`ca:127.0.0.1`, the fake Gateway, by default). */
export function deviceKey(device = DEVICE) {
  const { publicKey, privateKey } = generateKeyPairSync('ec', { namedCurve: 'P-256' });
  const jwk = publicKey.export({ format: 'jwk' });
  const raw = Buffer.concat([Buffer.from([4]), Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]);
  const fingerprint = createHash('sha256').update(raw).digest('hex').slice(0, 32);
  return { device, key: raw.toString('base64'), privateKey, fingerprint, entry: `${device}:${fingerprint}`,
    hello: (extra = {}) => ({ t: 'hello', v: 5, device, key: raw.toString('base64'), fw: '0.6.1', name: 'Кубик', volume: 100, ...extra }),
    sign: (nonce, { device: signedDevice = device, key = raw.toString('base64'), bind = FAKE_GATEWAY_BIND } = {}) =>
      sign('sha256', Buffer.from(`kubik-auth-v5\n${nonce}\n${signedDevice}\n${key}\n${bind}`, 'utf8'), { key: privateKey, dsaEncoding: 'der' }).toString('base64') };
}
export const DEFAULT_DEVICE_KEY = deviceKey();

/** Fake OpenClaw pairing store (runtime.channel.pairing); `approve(entry)` models `openclaw pairing approve`. */
export function fakePairing({ allowed = [], full = false } = {}) {
  const state = { allowed: [...allowed], full, upserts: [], reads: 0, codes: new Map() };
  const api = {
    upsertPairingRequest: async (params) => {
      state.upserts.push(params);
      if (state.full) return { code: '', created: false };
      const existing = state.codes.get(params.id);
      if (existing) return { code: existing, created: false };
      const code = `ABCD234${state.codes.size + 2}`;
      state.codes.set(params.id, code);
      return { code, created: true };
    },
    readAllowFromStore: async (params) => { state.reads++; state.lastRead = params; return [...state.allowed]; },
  };
  return { api, state, approve: (entry) => { state.allowed.push(entry); } };
}

/**
 * A device client that records every frame. `auth(nonce)` answers the challenge with a signature (default: the
 * default key over `options.bind`, `ca:<host of the URL>`; `bind: 'seen'` = the SPKI hash of the TLS certificate this client saw, as
 * a LAN device signs it); `fw` is the hello firmware (default 0.6.1, `undefined` = none). wss:// URLs skip CA validation like a device in LAN mode (it pins instead).
 */
function autoAcknowledge(ws, event, autoPlayed, autoShown, playedSamples) {
  const ack = event.t === 'speak_end' && autoPlayed ? { t: 'played', gen: event.gen, ms: Math.round((playedSamples.get(event.gen) ?? 0) / 24) } :
    event.t === 'text' && event.receipt && autoShown ? { t: 'shown', receipt: event.receipt } : null;
  if (ack) setTimeout(() => { if (ws.readyState === 1) ws.send(JSON.stringify(ack)); }, 10);
}

export async function connectDevice(url, options = {}) {
  const identity = options.hello === undefined ? DEFAULT_DEVICE_KEY : null;
  const hello = identity ? identity.hello({ fw: 'fw' in options ? options.fw : '0.6.1' }) : options.hello;
  const autoPlayed = options.autoPlayed ?? true;
  const autoShown = options.autoShown !== false;
  let seen;
  const bind = () => (options.bind === 'seen' ? seen : options.bind ?? caBindFor(url));
  const auth = options.auth ?? (identity ? (nonce) => identity.sign(nonce, { bind: bind() }) : undefined);
  const ws = new WebSocket(url, { headers: options.headers, ...(url.startsWith('wss:') ? { rejectUnauthorized: false } : {}) });
  ws.on('error', () => {}); // a refused handshake rejects the `open` wait below
  ws.on('upgrade', (response) => {
    const raw = response.socket.getPeerCertificate?.(true)?.raw;
    if (raw) seen = spkiSha256(raw);
  });
  const events = [];
  const audio = new Map(); // gen -> bytes
  const playedSamples = new Map(); // separate from wire bytes: excludes each IMA header
  const kinds = new Set(); // speech frame kinds seen
  const waiters = [];
  ws.on('message', (data, isBinary) => {
    if (isBinary) {
      kinds.add(data[0]); audio.set(data[1], (audio.get(data[1]) ?? 0) + data.length - 2);
      if (data[0] === 3) {
        playedSamples.set(data[1], (playedSamples.get(data[1]) ?? 0) + Math.max(0, data.length - 5) * 2);
        if (options.autoProgress !== false) ws.send(JSON.stringify({ t: 'progress', gen: data[1], ms: Math.floor(playedSamples.get(data[1]) / 24) }));
      }
      return;
    }
    const event = JSON.parse(data.toString());
    events.push(event);
    if (event.t === 'speak') playedSamples.set(event.gen, 0);
    if (auth && event.t === 'challenge') ws.send(JSON.stringify({ t: 'auth', sig: auth(event.nonce) }));
    autoAcknowledge(ws, event, autoPlayed, autoShown, playedSamples);
    for (const waiter of [...waiters]) if (waiter.match(event)) { waiters.splice(waiters.indexOf(waiter), 1); waiter.resolve(event); }
  });
  const closed = new Promise((resolve) => ws.once('close', (code) => resolve(code)));
  await once(ws, 'open');
  if (hello) ws.send(JSON.stringify(hello));
  const device = {
    ws, events, audio, kinds, closed, get peerSpki() { return seen; },
    receivedMs: gen => (playedSamples.get(gen) ?? 0) / 24,
    send: (message) => ws.send(JSON.stringify(message)),
    sendAudio: (turn, pcm) => {
      const encoder = new ImaEncoder();
      for (let off = 0; off < pcm.length; off += 1920)
        ws.send(Buffer.concat([Buffer.from([KIND_MIC, turn]), encoder.encode(pcm.subarray(off, off + 1920))]));
    },
    waitFor(match, timeoutMs = 5000, resolveOnClose = false) {
      const found = events.find(match);
      if (found) return Promise.resolve(found);
      if (resolveOnClose && ws.readyState === WebSocket.CLOSED) return Promise.resolve(null);
      return new Promise((resolve, reject) => {
        const cleanup = () => { clearTimeout(timer); ws.off('close', onClose); };
        const waiter = { match, resolve: (e) => { cleanup(); resolve(e); } };
        const onClose = () => {
          if (!resolveOnClose) return;
          waiters.splice(waiters.indexOf(waiter), 1);
          cleanup(); resolve(null);
        };
        const timer = setTimeout(() => { waiters.splice(waiters.indexOf(waiter), 1); cleanup(); reject(new Error(`timeout waiting; got ${JSON.stringify(events)}`)); }, timeoutMs);
        if (resolveOnClose) ws.once('close', onClose);
        waiters.push(waiter);
      });
    },
    /** Waits for the n-th (1-based) event matching `match` counted from the start. */
    async waitForNth(match, n, timeoutMs = 5000) {
      const start = Date.now();
      while (events.filter(match).length < n) {
        if (Date.now() - start > timeoutMs) throw new Error(`timeout waiting for #${n}; got ${JSON.stringify(events)}`);
        await sleep(5);
      }
      return events.filter(match)[n - 1];
    },
    close: () => ws.close(),
  };
  if (hello && auth && options.awaitAuth !== false) {
    const result = await device.waitFor((event) => ['welcome', 'pair', 'error'].includes(event.t), 5000, true);
    if (result?.t === 'welcome') await device.waitFor((event) => event.t === 'state' && event.s === 'idle');
  }
  return device;
}

export function installRuntime({ payloads = [{ text: 'Ответ' }], dispatchImpl, extraCore = {}, pairing } = {}) {
  const seen = { routes: [], contexts: [], sessions: [], dispatches: [], configWrites: [] };
  const configDraft = config();
  const core = {
    config: { mutateConfigFile: async ({ mutate, afterWrite }) => {
      await mutate(configDraft);
      seen.configWrites.push({ afterWrite, config: structuredClone(configDraft) });
      return { result: undefined };
    } },
    channel: {
      ...(pairing ? { pairing } : {}),
      routing: { resolveAgentRoute: (params) => { seen.routes.push(params); return {
        agentId: 'main', accountId: params.accountId, sessionKey: `agent:main:kubik:direct:${params.peer.id}` }; } },
      session: { resolveStorePath: () => '/test/session.json', readSessionUpdatedAt: () => undefined,
        recordInboundSession: async (params) => { seen.sessions.push(params); } },
      reply: {
        resolveEnvelopeFormatOptions: () => ({}),
        formatAgentEnvelope: ({ channel, from, body }) => `[${channel} ${from}] ${body}`,
        finalizeInboundContext: (ctx) => { seen.contexts.push(ctx); return ctx; },
        dispatchReplyWithBufferedBlockDispatcher: async (params) => {
          seen.dispatches.push(params);
          if (dispatchImpl) return dispatchImpl(params);
          for (const payload of payloads) await params.dispatcherOptions.deliver(payload, { kind: payload.kind ?? 'block' });
          return undefined;
        },
      },
    },
    ...extraCore,
  };
  const sdk = {
    replyPrefix: () => ({ onModelSelected: () => {} }),
    channelReadyPatch: (extras = {}) => ({ running: true, connected: true, lifecycle: 'ready', lastError: null, ...extras }),
    channelStoppedPatch: (extras = {}) => ({ running: false, connected: false, lifecycle: 'stopped', ...extras }),
    transportActivityPatch: (at = Date.now()) => ({ lastTransportActivityAt: at }),
  };
  setRuntime(core, sdk);
  return { core, sdk, seen };
}

/** Fake core speech runtime; records calls and lets tests hold requests open. */
export function fakeCore({ transcript = 'Привет, Кубик', outcome = 'success', telephony } = {}) {
  const calls = { stt: [], tts: [] };
  const core = {
    modelAuth: { resolveApiKeyForProvider: async () => ({ apiKey: 'test-key', mode: 'api-key' }) },
    mediaUnderstanding: {
      transcribeAudioFile: async (params) => {
        const wav = parseWav(readFileSync(params.filePath));
        calls.stt.push({ ...params, sampleRate: wav.sampleRate, bytes: wav.pcm.length });
        if (core.holdStt) await core.holdStt;
        return { text: transcript, provider: 'openai', model: 'gpt-4o-mini-transcribe', decision: { capability: 'audio', outcome, attachments: [] } };
      },
    },
    tts: {
      textToSpeechTelephony: async (params) => {
        calls.tts.push(params);
        if (core.holdTts) await core.holdTts;
        return telephony ? telephony(params) : { success: true, provider: 'openai', audioBuffer: tone(500), outputFormat: 'pcm', sampleRate: 24000 };
      },
    },
  };
  return { core, calls };
}

export function acknowledgeNativeInput(session, value) {
  if (typeof value !== 'string') return;
  const event = JSON.parse(value);
  if (event.t === 'live_input') queueMicrotask(() => session.handleMessage({ t: 'live_input_ack', turn: event.turn, on: event.on }));
}
