// Kubik device protocol v5. Pure functions: no sockets, no timers.
import { createHash, createPublicKey, X509Certificate } from 'node:crypto';

export const PROTOCOL_VERSION = 5;
export const AUTH_CONTEXT = 'kubik-auth-v5';
/** Transport binding a device signs when it validated the server by a public CA (gateway route): `ca:<host>` it connected to. */
export const caBind = (host) => `ca:${host}`;
/** Longest host in a `ca:<host>` bind (the whole bind is at most 127 characters). */
export const MAX_BIND_HOST = 123;
export const DEVICE_PATH = '/kubik/v1';
/** Default TCP port of the LAN TLS listener and the fixed UDP port of its discovery responder. */
export const LAN_PORT = 18790;
export const DISCOVERY_REQUEST = 'kubik-discover-v5';
export const MAX_JSON_BYTES = 4096;
export const MAX_AUDIO_FRAME_BYTES = 8192;
export const SPEECH_LEAD_MS = 800; // Firmware guarantees 1100 ms, including its 400 ms prebuffer.
export const MAX_SPEECH_PAYLOAD_BYTES = 4800; // 100 ms of 24 kHz s16le mono before IMA encoding
export const SAMPLE_RATE = 24000;
export const BYTES_PER_MS = (SAMPLE_RATE * 2) / 1000; // 48
export const MAX_NATIVE_AUDIO_BYTES = 60_000 * BYTES_PER_MS; // host queue, including any single provider delta
export const KIND_MIC = 0x04;
export const KIND_MIC_ECHO = 0x05;
export const KIND_SPEECH_IMA = 0x03;
export const CLOSE = Object.freeze({ NORMAL: 1000, GOING_AWAY: 1001, UNAUTHORIZED: 4001, PROTOCOL: 4002, REPLACED: 4003,
  PAIRING_TIMEOUT: 4004, PAIRING_BUSY: 4005, INTERNAL: 1011 });
export const POKE_KINDS = Object.freeze(['tap', 'pet', 'shake', 'pickup']);

export class ProtocolError extends Error {
  constructor(message) { super(message); this.name = 'ProtocolError'; }
}

const u8 = (value) => Number.isInteger(value) && value >= 0 && value <= 255;
const uint = (value) => Number.isSafeInteger(value) && value >= 0;
const shortString = (value, max = 256) => typeof value === 'string' && value.length > 0 && value.length <= max;
/** Strict standard base64 (padded, canonical): the decoded bytes, or null. */
function base64(value, max) {
  if (!shortString(value, max) || !/^[A-Za-z0-9+/]+={0,2}$/.test(value)) return null;
  const bytes = Buffer.from(value, 'base64');
  return bytes.length && bytes.toString('base64') === value ? bytes : null;
}

/**
 * Decodes a device public key: base64 of the 65-byte uncompressed SEC1 P-256 point (0x04 || X || Y).
 * Returns `{ raw, publicKey, fingerprint }`; throws ProtocolError when it is not a valid curve point.
 * `fingerprint` = first 32 hex chars of sha256(raw).
 */
export function decodeDeviceKey(key) {
  const raw = base64(key, 128);
  if (!raw || raw.length !== 65 || raw[0] !== 0x04) throw new ProtocolError('key must be a base64 uncompressed P-256 point');
  let publicKey;
  try {
    publicKey = createPublicKey({ format: 'jwk', key: { kty: 'EC', crv: 'P-256',
      x: raw.subarray(1, 33).toString('base64url'), y: raw.subarray(33).toString('base64url') } });
  } catch { throw new ProtocolError('key is not a valid P-256 point'); }
  return { raw, publicKey, fingerprint: createHash('sha256').update(raw).digest('hex').slice(0, 32) };
}

/**
 * The exact bytes a v5 device signs (ECDSA P-256, SHA-256): wire strings joined by newlines. `bind` is the transport
 * the device saw: the hex SHA-256 of the server SPKI (LAN) or `ca:<host>`; the server checks it against its own. (`none`,
 * for plain ws:// development servers, is signed by devices but never accepted here.)
 */
export const authMessage = ({ nonce, device, key, bind }) =>
  Buffer.from(`${AUTH_CONTEXT}\n${nonce}\n${device}\n${key}\n${bind}`, 'utf8');

/** Lowercase hex SHA-256 of a DER certificate's SubjectPublicKeyInfo: what a LAN device pins and signs as `bind`. */
export function spkiSha256(certificateDer) {
  const spki = new X509Certificate(certificateDer).publicKey.export({ type: 'spki', format: 'der' });
  return createHash('sha256').update(spki).digest('hex');
}

// hello.name: shown in logs and pairing requests; no control, format (bidi) or unassigned characters.
const DISPLAY_NAME = /^(?=.*\S)[^\p{C}]{1,32}$/u;
const FIRMWARE_VERSION = /^[\x21-\x7e]{1,32}$/;

// Validation stays ordered for stable errors; each entry also defines the canonical output shape.
const DEVICE_MESSAGE_PARSERS = new Map([
  ['hello', {
    checks: [
      [(message) => message.v === PROTOCOL_VERSION, 'unsupported protocol version'],
      [(message) => message.fw === undefined || (typeof message.fw === 'string' && FIRMWARE_VERSION.test(message.fw)), 'invalid fw'],
      [(message) => message.name === undefined || (typeof message.name === 'string' && DISPLAY_NAME.test(message.name)), 'invalid name'],
      [(message) => message.volume === undefined || (Number.isInteger(message.volume) && message.volume >= 0 && message.volume <= 100), 'invalid volume'],
      [(message) => shortString(message.device, 64) && typeof message.key === 'string', 'hello needs device and key'],
    ],
    normalize: (message) => {
      decodeDeviceKey(message.key);
      return { t: 'hello', v: PROTOCOL_VERSION, device: message.device, key: message.key, fw: message.fw, name: message.name,
        ...(message.volume !== undefined ? { volume: message.volume } : {}) };
    },
  }],
  ['auth', {
    checks: [[(message) => Boolean(base64(message.sig, 256)), 'auth needs sig (base64)']],
    normalize: (message) => ({ t: 'auth', sig: message.sig }),
  }],
  ['ptt', {
    checks: [
      [(message) => typeof message.on === 'boolean' && u8(message.turn), 'ptt needs on and turn (u8)'],
      [(message) => message.ms === undefined || uint(message.ms), 'invalid ms'],
      [(message) => message.automatic === undefined || (message.on && typeof message.automatic === 'boolean'), 'invalid automatic'],
    ],
    normalize: (message) => ({ t: 'ptt', on: message.on, turn: message.turn, ms: message.ms,
      ...(message.automatic !== undefined ? { automatic: message.automatic } : {}) }),
  }],
  ['live_input_ack', {
    checks: [[message => typeof message.on === 'boolean' && u8(message.turn), 'live_input_ack needs on and turn']],
    normalize: message => ({ t: 'live_input_ack', on: message.on, turn: message.turn }),
  }],
  ['cancel', {
    checks: [[(message) => message.gen === undefined || u8(message.gen), 'invalid gen'],
      [(message) => message.turn === undefined || (u8(message.turn) && message.gen === undefined), 'invalid cancel turn']],
    normalize: (message) => ({ t: 'cancel', gen: message.gen,
      ...(message.turn !== undefined ? { turn: message.turn } : {}) }),
  }],
  ['played', {
    checks: [
      [(message) => u8(message.gen), 'played needs gen (u8)'],
      [(message) => uint(message.ms), 'played needs ms'],
    ],
    normalize: (message) => ({ t: 'played', gen: message.gen, ms: message.ms }),
  }],
  ['cancelled', {
    checks: [[message => u8(message.gen), 'cancelled needs gen']],
    normalize: message => ({ t: 'cancelled', gen: message.gen }),
  }],
  ['shown', {
    checks: [[(message) => uint(message.receipt) && message.receipt > 0 && message.receipt <= 0xffffffff, 'shown needs receipt']],
    normalize: (message) => ({ t: 'shown', receipt: message.receipt }),
  }],
  ['progress', {
    checks: [[(message) => u8(message.gen) && uint(message.ms), 'progress needs gen (u8) and ms']],
    normalize: (message) => ({ t: 'progress', gen: message.gen, ms: message.ms }),
  }],
  ['poke', {
    checks: [[(message) => typeof message.kind === 'string' && /^[a-z_]{1,16}$/.test(message.kind), 'invalid poke kind']],
    normalize: (message) => ({ t: 'poke', kind: POKE_KINDS.includes(message.kind) ? message.kind : 'other' }),
  }],
  ['device_state', {
    checks: [[(message) => Number.isInteger(message.volume) && message.volume >= 0 && message.volume <= 100, 'invalid volume']],
    normalize: (message) => ({ t: 'device_state', volume: message.volume }),
  }],
  ['agent_options', {
    checks: [
      [(message) => ['agent', 'mode', 'voice', 'stt', 'tts'].includes(message.target), 'invalid model target'],
      [(message) => Number.isInteger(message.rid) && message.rid >= 0 && message.rid <= 65535, 'invalid rid'],
      [(message) => message.cursor === undefined || u8(message.cursor), 'invalid cursor'],
    ],
    normalize: (message) => ({ t: 'agent_options', target: message.target, rid: message.rid, cursor: message.cursor ?? 0 }),
  }],
  ['agent_model', {
    checks: [
      [(message) => ['agent', 'mode', 'voice', 'stt', 'tts'].includes(message.target), 'invalid model target'],
      [(message) => Number.isInteger(message.rid) && message.rid >= 0 && message.rid <= 65535, 'invalid rid'],
      [(message) => u8(message.cursor), 'invalid cursor'],
      [(message) => shortString(message.id, 160) && !/[\p{C}]/u.test(message.id), 'invalid model id'],
    ],
    normalize: (message) => ({ t: 'agent_model', target: message.target, rid: message.rid, cursor: message.cursor, id: message.id }),
  }],
  ['ping', {
    checks: [[(message) => message.ts === undefined || (typeof message.ts === 'number' && Number.isFinite(message.ts)), 'invalid ts']],
    normalize: (message) => ({ t: 'ping', ts: message.ts }),
  }],
]);

/** Validates one device → server JSON text frame (a `ws` message Buffer). Throws ProtocolError on anything unexpected. */
export function parseDeviceMessage(data) {
  if (data.length > MAX_JSON_BYTES) throw new ProtocolError('JSON frame exceeds 4 KiB');
  let message;
  try { message = JSON.parse(data.toString('utf8')); }
  catch { throw new ProtocolError('invalid JSON'); }
  if (message === null || typeof message !== 'object' || Array.isArray(message)) throw new ProtocolError('frame must be an object');
  const parser = DEVICE_MESSAGE_PARSERS.get(message.t);
  if (!parser) throw new ProtocolError('unknown frame type');
  for (const [valid, error] of parser.checks) {
    if (!valid(message)) throw new ProtocolError(error);
  }
  return parser.normalize(message);
}

/** Decode independently framed 40 ms mono input, or Live mic + codec reference. */
export function parseAudioFrame(data) {
  if (data.length < 6) throw new ProtocolError('audio frame too short');
  const kind = data[0], turn = data[1];
  if (kind === KIND_MIC_ECHO) {
    if (data.length !== 968) throw new ProtocolError('Live echo frame must contain two 40 ms channels');
    return { kind, turn, pcm: decodeMic(data.subarray(2, 485)), reference: decodeMic(data.subarray(485)) };
  }
  if (kind !== KIND_MIC) throw new ProtocolError('unexpected binary frame kind');
  if (data.length > 485) throw new ProtocolError('microphone frame exceeds 40 ms');
  return { kind, turn, pcm: decodeMic(data.subarray(2)) };
}
function decodeMic(data) {
  if (data[2] > 88) throw new ProtocolError('invalid IMA step index');
  const decoder = new ImaEncoder();
  decoder.pred = data.readInt16LE(0); decoder.index = data[2];
  const pcm = Buffer.allocUnsafe((data.length - 3) * 4);
  for (let i = 3; i < data.length; i++) {
    decoder.step(data[i] & 15); pcm.writeInt16LE(decoder.pred, (i - 3) * 4);
    decoder.step(data[i] >> 4); pcm.writeInt16LE(decoder.pred, (i - 3) * 4 + 2);
  }
  return pcm;
}

const IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
  97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
  963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
  6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];
const IMA_INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8];

/**
 * IMA ADPCM (4 bits/sample) for speech: a quarter of the PCM bytes, so the device ring holds 1100 ms and
 * the link carries 12 KB/s. One encoder per gen; each frame starts with the state before it (int16 LE predictor,
 * u8 step index), then the nibbles, low one first. Identical to firmware/main/ima_adpcm.c.
 */
export class ImaEncoder {
  pred = 0;
  index = 0;
  /** Advances the predictor by one nibble; a decoder (the device, or a test) runs exactly this. */
  step(nibble) {
    const step = IMA_STEP[this.index];
    let diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    this.pred = Math.max(-32768, Math.min(32767, this.pred + (nibble & 8 ? -diff : diff)));
    this.index = Math.max(0, Math.min(88, this.index + IMA_INDEX_ADJ[nibble & 7]));
  }
  #sample(sample) {
    let step = IMA_STEP[this.index];
    let diff = sample - this.pred;
    let nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    if (diff >= step) { nibble |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { nibble |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) nibble |= 1;
    this.step(nibble);
    return nibble;
  }
  /** s16le PCM → one frame payload. An odd last sample is repeated to fill the byte. */
  encode(pcm) {
    const samples = pcm.length >> 1;
    const out = Buffer.allocUnsafe(3 + ((samples + 1) >> 1));
    out.writeInt16LE(this.pred, 0);
    out[2] = this.index;
    for (let i = 0; i < samples; i += 2) {
      const lo = this.#sample(pcm.readInt16LE(i * 2));
      const hi = this.#sample(pcm.readInt16LE(Math.min(i + 1, samples - 1) * 2));
      out[3 + (i >> 1)] = lo | (hi << 4);
    }
    return out;
  }
}

export function speechFrameIma(gen, payload) {
  const frame = Buffer.allocUnsafe(payload.length + 2);
  frame[0] = KIND_SPEECH_IMA; frame[1] = gen;
  payload.copy(frame, 2);
  return frame;
}

/** Server → device JSON builders. Only `text` (agent output) and `pair` (code from the pairing store) are checked. */
export const out = {
  // `progress`: this server takes playback "progress" reports (older ones close the link on unknown frames).
  welcome: (session, volume) => ({ t: 'welcome', session, progress: true, ...(volume !== undefined ? { volume } : {}) }),
  state: (s) => ({ t: 'state', s }),
  emotion: (e, ms) => ({ t: 'emotion', e, ...(ms ? { ms } : {}) }),
  speak: (gen, kind) => ({ t: 'speak', gen, kind }),
  speakEnd: (gen) => ({ t: 'speak_end', gen }),
  speakCancel: gen => ({ t: 'speak_cancel', gen }),
  text: (text, kind, receipt) => {
    if (typeof text !== 'string' || !text || Buffer.byteLength(text) > 1000) throw new RangeError('bad text');
    return { t: 'text', text, kind, ...(receipt !== undefined ? { receipt } : {}) };
  },
  error: (code) => ({ t: 'error', code }),
  // Cron jobs: how many run now, and seconds until the next one-shot job (-1 = none).
  cron: (running, next) => ({ t: 'cron', running, next }),
  // What OpenClaw is busy with: `own` = a run in this device's own session (its question, or a heartbeat or
  // cron result on its way to it), `other` = any other run in the Gateway. '' = nothing. Older firmware ignores it.
  activity: (own, other) => ({ t: 'activity', own, other }),
  pong: (ts) => ({ t: 'pong', ...(ts !== undefined ? { ts } : {}) }),
  challenge: (nonce) => ({ t: 'challenge', nonce }),
  pair: (code) => {
    if (!/^[A-Z0-9]{0,16}$/.test(code)) throw new RangeError('bad pairing code');
    return { t: 'pair', code };
  },
};

export const pcmDurationMs = (bytes) => bytes / BYTES_PER_MS;
/** Next generation id; 0 is never used so a device can treat it as "none". */
export const nextGen = (gen) => (gen % 255) + 1;
