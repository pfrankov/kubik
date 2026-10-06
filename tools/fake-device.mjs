#!/usr/bin/env node
import { ImaEncoder, KIND_MIC } from '../openclaw-kubik/src/protocol.js';
// Fake Kubik device: speaks the Kubik device protocol v5 to the OpenClaw plugin.
//
//   node tools/fake-device.mjs [--turns 3] [--silent] [--cancel-after MS] [--cancel-turn N]
//                              [--stay MS] [--url kubik://127.0.0.1:18790] [--device kubik-b6c634]
//                              [--key-file /path/key.pem] [--approve-local] [--phrases "Фраза один|Фраза два"]
//
// Each turn holds PTT for ~2 s of real speech (macOS `say`, else a tone) streamed in 40 ms frames at
// real-time pace, then waits for the reply. Prints one JSON line per event with timestamps, saves the
// received speech to tools/.out/turn-N.wav (notifications: notify-N.wav) and sends `played` when the
// simulated playback ends. The persistent P-256 test key is kept in a gitignored local file.
// Server forms as on the device: kubik://host[:port] is the plugin's LAN TLS listener (the auth binds the SPKI
// hash of the certificate it presented); wss://host/kubik/v1 is the Gateway route behind a CA certificate (`ca:<host>`);
// ws://127.0.0.1:<gateway port>/kubik/v1 reaches the Gateway route locally and needs --bind ca:127.0.0.1.
import { execFileSync } from 'node:child_process';
import { createPrivateKey, createPublicKey, generateKeyPairSync, sign } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';
import { serverTarget, spkiSha256 } from './usb-bridge/bridge-protocol.mjs';
import { createSynth } from './mock-openai/server.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, '.out');
const FRAME_MS = 40;
const FRAME_BYTES = FRAME_MS * 48; // 1920
const argv = process.argv.slice(2);
const flag = (name) => argv.includes(`--${name}`);
const opt = (name, fallback) => { const i = argv.indexOf(`--${name}`); return i >= 0 && argv[i + 1] !== undefined ? argv[i + 1] : fallback; };
if (flag('help')) { console.log(readHelp()); process.exit(0); }

const secrets = JSON.parse(await readFile(join(HERE, 'dev-secrets.json'), 'utf8').catch(() => '{}'));
const localUrl = process.env.KUBIK_DEVICE_PORT ? `kubik://127.0.0.1:${process.env.KUBIK_DEVICE_PORT}` : secrets.url;
const url = opt('url', localUrl ?? 'kubik://127.0.0.1:18790');
const target = serverTarget(url);
const bindOverride = opt('bind');
let bind = null; // what this connection's auth signs
const deviceId = opt('device', secrets.deviceId ?? 'kubik-b6c634');
const keyFile = opt('key-file', join(HERE, 'dev-device-key.pem'));
const turns = Number(opt('turns', 3));
const silent = flag('silent');
const cancelAfter = opt('cancel-after') !== undefined ? Number(opt('cancel-after')) : null;
const cancelTurn = Number(opt('cancel-turn', turns));
const stayMs = Number(opt('stay', 0));
const utterMs = Number(opt('utter-ms', 2000));
let privateKey;
try {
  privateKey = createPrivateKey(await readFile(keyFile));
} catch (error) {
  if (error.code !== 'ENOENT') throw error;
  privateKey = generateKeyPairSync('ec', { namedCurve: 'prime256v1' }).privateKey;
  await mkdir(dirname(keyFile), { recursive: true });
  await writeFile(keyFile, privateKey.export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600, flag: 'wx' });
}
const jwk = createPublicKey(privateKey).export({ format: 'jwk' });
const deviceKey = Buffer.concat([Buffer.from([4]), Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]).toString('base64');

const t0 = performance.now();
const ms = () => Math.round(performance.now() - t0);
const emit = (ev, data = {}) => console.log(JSON.stringify({ t: ms(), ev, ...data }));
const sleep = (t) => new Promise((r) => setTimeout(r, t));

// ---- audio helpers --------------------------------------------------------------------------------------
function wav(pcm) {
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + pcm.length, 4); h.write('WAVE', 8); h.write('fmt ', 12); h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22); h.writeUInt32LE(24000, 24); h.writeUInt32LE(48000, 28); h.writeUInt16LE(2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([h, pcm]);
}
/** Duration, overall RMS and "voiced" time (20 ms windows above an RMS threshold) of s16le PCM. */
function analyse(pcm) {
  const n = Math.floor(pcm.length / 2);
  let sum = 0; let voiced = 0; let peak = 0;
  for (let w = 0; w < n; w += 480) {
    let ws = 0; const end = Math.min(n, w + 480);
    for (let i = w; i < end; i++) { const v = pcm.readInt16LE(i * 2) / 32768; ws += v * v; peak = Math.max(peak, Math.abs(v)); }
    sum += ws;
    if (Math.sqrt(ws / (end - w)) > 0.02) voiced += (end - w) / 24;
  }
  return { durationMs: Math.round(n / 24), rms: n ? Number(Math.sqrt(sum / n).toFixed(3)) : 0, peak: Number(peak.toFixed(3)), voicedMs: Math.round(voiced) };
}
const DEFAULT_PHRASES = ['Привет, Кубик! Как у тебя дела?', 'Кубик, какая завтра погода?', 'Расскажи мне, пожалуйста, шутку.',
  'Напомни мне через пять минут выпить воды.', 'Мне сегодня что-то грустно.', 'Кубик, а что ты умеешь?'];
const PHRASES = opt('phrases') ? opt('phrases').split('|').map((p) => p.trim()).filter(Boolean) : DEFAULT_PHRASES;
const synthesize = createSynth({ log: (m) => emit('mic_synth_note', { message: m }) });
async function micAudio(i) {
  const bytes = Math.round(utterMs * 48);
  if (silent) return Buffer.alloc(bytes);
  const speech = await synthesize(PHRASES[(i - 1) % PHRASES.length]);
  const pcm = Buffer.alloc(Math.max(bytes, speech.length + 9600)); // 100 ms lead-in, padded to at least utterMs
  speech.copy(pcm, 4800, 0, Math.min(speech.length, pcm.length - 4800));
  return pcm;
}

// ---- connection -----------------------------------------------------------------------------------------
await mkdir(OUT, { recursive: true });
let ws;
const listeners = new Set();
const pendingEmotions = [];
const gens = new Map(); // gen -> { kind, chunks, firstAt, bytes, turn, emotions }
let notifyCount = 0;
let currentTurn = null;
let closedCode = null;
let approvedLocally = false;
const summary = { turns: [], notifies: [], errors: [] };

function next(match, timeoutMs = 60_000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { listeners.delete(listener); reject(new Error(`timeout after ${timeoutMs} ms`)); }, timeoutMs);
    const listener = (event) => { if (match(event)) { clearTimeout(timer); listeners.delete(listener); resolve(event); } };
    listeners.add(listener);
  });
}
const send = (message) => ws.send(JSON.stringify(message));

function receiveAudio(data) {
  const gen = data[1];
  const g = gens.get(gen);
  if (!g || g.cancelled) return;
  if (!g.firstAt) {
    g.firstAt = performance.now();
    emit('first_audio', { gen, afterPttOffMs: g.turn?.offAt ? Math.round(g.firstAt - g.turn.offAt) : undefined });
    if (g.onFirstAudio) g.onFirstAudio();
  }
  g.chunks.push(Buffer.from(data.subarray(2)));
  g.bytes += data.length - 2;
  g.frames++;
  g.maxFrame = Math.max(g.maxFrame, data.length - 2);
  // Buffer model: device plays from first byte at real time; how far ahead of playback is the server?
  const playedMs = performance.now() - g.firstAt;
  g.maxAheadMs = Math.max(g.maxAheadMs, g.bytes / 48 - playedMs);
}

function answerChallenge(event) {
  const message = Buffer.from(`kubik-auth-v5\n${event.nonce}\n${deviceId}\n${deviceKey}\n${bind}`, 'utf8');
  send({ t: 'auth', sig: sign('sha256', message, privateKey).toString('base64') });
}

function approvePairing(event) {
  if (!flag('approve-local') || !event.code) return;
  const openclaw = join(HERE, '../openclaw-kubik/node_modules/.bin/openclaw');
  execFileSync(openclaw, ['--profile', 'kubik', 'pairing', 'approve', 'kubik', event.code], { stdio: 'pipe' });
  approvedLocally = true;
  emit('pair_approved_local', { code: event.code });
}

function startGeneration(event) {
  const g = { kind: event.kind, chunks: [], bytes: 0, frames: 0, maxFrame: 0, maxAheadMs: 0, firstAt: 0,
    turn: event.kind === 'reply' ? currentTurn : null, emotions: pendingEmotions.splice(0) };
  gens.set(event.gen, g);
  if (g.turn) g.turn.gen = event.gen;
  if (g.turn && g.turn.index === cancelTurn && cancelAfter !== null) {
    g.onFirstAudio = () => setTimeout(() => {
      emit('send_cancel', { gen: event.gen, afterFirstAudioMs: cancelAfter });
      g.cancelled = true;
      send({ t: 'cancel', gen: event.gen });
    }, cancelAfter);
  }
}

function recordEmotion(event) {
  const open = [...gens.values()].filter((g) => !g.done);
  if (open.length) for (const g of open) g.emotions.push(event.e);
  else pendingEmotions.push(event.e); // the opening emotion is sent right before `speak`
}

async function finishGeneration(event) {
  const g = gens.get(event.gen);
  if (!g) return {};
  g.done = true;
  const pcm = Buffer.concat(g.chunks);
  const name = g.kind === 'notify' ? `notify-${++notifyCount}.wav` : `turn-${g.turn?.index ?? 'x'}.wav`;
  await writeFile(join(OUT, name), wav(pcm));
  const stats = analyse(pcm);
  const extra = { file: `tools/.out/${name}`, ...stats, frames: g.frames, maxFrameBytes: g.maxFrame,
    maxAheadMs: Math.round(g.maxAheadMs), emotions: g.emotions };
  const record = { gen: event.gen, kind: g.kind, file: extra.file, ...stats, maxAheadMs: extra.maxAheadMs, emotions: g.emotions };
  if (g.kind === 'notify') summary.notifies.push(record); else if (g.turn) g.turn.speech = record;
  // Simulated playback: `played` when the last buffered sample would have been played.
  const playEnd = g.firstAt + stats.durationMs;
  setTimeout(() => { if (ws.readyState === 1) { send({ t: 'played', gen: event.gen, ms: stats.durationMs }); emit('sent_played', { gen: event.gen }); } },
    Math.max(0, playEnd - performance.now()));
  return extra;
}

const eventHandlers = new Map([
  ['challenge', answerChallenge], ['pair', approvePairing], ['speak', startGeneration],
  ['emotion', recordEmotion], ['speak_end', finishGeneration],
  ['error', (event) => summary.errors.push({ turn: currentTurn?.index, code: event.code })],
]);

async function receiveEvent(data) {
  const event = JSON.parse(data.toString('utf8'));
  const extra = (await eventHandlers.get(event.t)?.(event)) ?? {};
  const { t: _type, ...fields } = event;
  emit(`recv_${event.t}`, { ...fields, ...extra });
  for (const listener of [...listeners]) listener(event);
}

function handleMessageError(error) {
  emit('client_error', { message: error.message });
  summary.errors.push({ turn: currentTurn?.index, code: 'client_error' });
  process.exitCode = 1;
  if (ws.readyState === 1) ws.close(1011, 'fake-device error');
}

function receiveMessage(data, isBinary) {
  if (isBinary) return receiveAudio(data);
  return receiveEvent(data).catch(handleMessageError);
}

async function connect() {
  if (target.discover) throw new Error('fake-device needs --url (no LAN discovery here)');
  const pinned = target.bind === 'spki';
  const socket = new WebSocket(target.url, { perMessageDeflate: false, ...(pinned ? { rejectUnauthorized: false } : {}) });
  ws = socket;
  closedCode = null;
  bind = bindOverride ?? (pinned ? null : target.bind);
  if (pinned && !bindOverride) socket.once('upgrade', (response) => { bind = spkiSha256(response.socket.getPeerCertificate().raw); });
  socket.on('message', receiveMessage);
  socket.on('close', (code, reason) => {
    closedCode = code;
    emit('closed', { code, reason: reason.toString() });
    for (const listener of [...listeners]) listener({ t: '__closed__', code });
  });
  socket.on('error', (error) => emit('socket_error', { message: error.message }));
  await new Promise((resolve, reject) => { socket.once('open', resolve); socket.once('error', reject); });
  emit('connected', { url: target.url, device: deviceId, bind: bind?.slice(0, 16) });
  const welcome = next((event) => event.t === 'welcome' || event.t === '__closed__', 120_000);
  send({ t: 'hello', v: 5, device: deviceId, key: deviceKey, fw: '0.6.1', name: 'Fake Kubik' });
  return welcome;
}

async function receiveWelcome() {
  let welcome = await connect();
  for (let attempt = 1; welcome.t !== 'welcome' && approvedLocally && attempt <= 5; attempt++) {
    emit('reconnect_after_pairing_restart', { attempt });
    await sleep(500);
    try { welcome = await connect(); }
    catch { await sleep(500); }
  }
  if (welcome.t !== 'welcome') { emit('rejected', { code: closedCode }); process.exit(1); }
}

await receiveWelcome();

// ---- turns ----------------------------------------------------------------------------------------------
for (let i = 1; i <= turns; i++) {
  const pcm = await micAudio(i);
  const turnId = i % 256;
  const turn = { index: i, turnId, silent };
  currentTurn = turn;
  summary.turns.push(turn);
  const idle = next((e) => e.t === '__closed__' || (e.t === 'state' && e.s === 'idle' && turn.offAt), 120_000);
  send({ t: 'ptt', on: true, turn: turnId });
  emit('ptt_on', { turn: turnId, audioMs: Math.round(pcm.length / 48), source: silent ? 'silence' : 'say' });
  const encoder = new ImaEncoder();
  const start = performance.now();
  for (let off = 0, n = 0; off < pcm.length; off += FRAME_BYTES, n++) {
    const wait = start + n * FRAME_MS - performance.now();
    if (wait > 0) await sleep(wait);
    ws.send(Buffer.concat([Buffer.from([KIND_MIC, turnId]), encoder.encode(pcm.subarray(off, off + FRAME_BYTES))]));
  }
  turn.offAt = performance.now();
  send({ t: 'ptt', on: false, turn: turnId, ms: Math.round(turn.offAt - start) });
  emit('ptt_off', { turn: turnId });
  const thinking = next((e) => e.t === 'state' && (e.s === 'thinking') || e.t === 'error', 60_000).then(() => {
    turn.transcriptMs = Math.round(performance.now() - turn.offAt);
    emit('transcribed', { turn: turnId, afterPttOffMs: turn.transcriptMs });
  }).catch(() => {});
  const end = await idle.catch((error) => ({ t: 'timeout', error }));
  await thinking;
  const g = turn.gen != null ? gens.get(turn.gen) : null;
  turn.firstAudioMs = g?.firstAt ? Math.round(g.firstAt - turn.offAt) : null;
  turn.totalMs = Math.round(performance.now() - turn.offAt);
  if (end.t !== 'state') { emit('turn_incomplete', { turn: turnId, reason: end.t }); break; }
  emit('turn_done', { turn: turnId, transcriptMs: turn.transcriptMs, firstAudioMs: turn.firstAudioMs, totalMs: turn.totalMs, cancelled: Boolean(g?.cancelled) });
  await sleep(300);
}
currentTurn = null;
if (stayMs > 0) { emit('listening_for_notifications', { forMs: stayMs }); await sleep(stayMs); }
emit('summary', { turns: summary.turns.map(({ offAt, ...t }) => t), notifies: summary.notifies, errors: summary.errors });
ws.close(1000);
await sleep(100);
process.exit(process.exitCode ?? 0);

function readHelp() {
  return fileURLToPath(import.meta.url) && `Fake Kubik device
  --turns N          push-to-talk turns (default 3)
  --silent           send silence (expects error stt_empty)
  --cancel-after MS  send cancel MS after the first audio of --cancel-turn (default: last turn)
  --stay MS          keep the connection open afterwards to receive notifications
  --utter-ms MS      minimum utterance length (default 2000)
  --key-file FILE    persistent P-256 private key (default tools/dev-device-key.pem)
  --approve-local   approve the test device in the local kubik OpenClaw profile
  --phrases "A|B"     what to say (macOS say -v Milena), one phrase per turn, cycled
  --url / --device   override the local profile defaults (kubik://HOST[:PORT] | wss://HOST/kubik/v1)
  --bind ca:HOST|HEX sign this transport binding instead of the one derived from --url`;
}
