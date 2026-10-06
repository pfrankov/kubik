import assert from 'node:assert/strict';
import test from 'node:test';
import { SpeechPacer } from '../src/audio.js';
import { verify } from 'node:crypto';
import { authMessage, caBind, CLOSE, decodeDeviceKey, ImaEncoder, nextGen, out, parseAudioFrame, parseDeviceMessage, ProtocolError,
  speechFrameIma } from '../src/protocol.js';
import { imaDecode } from './audio-analysis.js';
import { deviceKey } from './helpers.js';

const parse = (value) => parseDeviceMessage(typeof value === 'string' ? value : JSON.stringify(value));

test('valid device frames parse to normalised objects', () => {
  assert.throws(() => parse({ t: 'hello', v: 1, device: 'kubik-1', token: 'old-token' }), /unsupported protocol version/);
  assert.throws(() => parse({ t: 'hello', v: 2, device: 'kubik-1', token: 'old-token' }), /unsupported protocol version/);
  assert.deepEqual(parse({ t: 'ptt', on: true, turn: 3 }), { t: 'ptt', on: true, turn: 3, ms: undefined });
  assert.deepEqual(parse({ t: 'ptt', on: false, turn: 255, ms: 1800 }), { t: 'ptt', on: false, turn: 255, ms: 1800 });
  assert.deepEqual(parse({ t: 'cancel', gen: 2 }), { t: 'cancel', gen: 2 });
  assert.deepEqual(parse({ t: 'cancel' }), { t: 'cancel', gen: undefined });
  assert.deepEqual(parse({ t: 'ptt', on: true, turn: 255, automatic: true }),
    { t: 'ptt', on: true, turn: 255, ms: undefined, automatic: true });
  assert.deepEqual(parse({ t: 'cancel', turn: 255 }), { t: 'cancel', gen: undefined, turn: 255 });
  for (const message of [{ t: 'ptt', on: true, turn: 1, automatic: 'yes' },
    { t: 'ptt', on: false, turn: 1, automatic: true }, { t: 'cancel', turn: 256 },
    { t: 'cancel', turn: 1, gen: 2 }]) assert.throws(() => parse(message), ProtocolError);
  assert.deepEqual(parse({ t: 'played', gen: 7, ms: 900 }), { t: 'played', gen: 7, ms: 900 });
  assert.deepEqual(parse({ t: 'progress', gen: 7, ms: 250 }), { t: 'progress', gen: 7, ms: 250 });
  assert.deepEqual(parse({ t: 'poke', kind: 'tap' }), { t: 'poke', kind: 'tap' });
  assert.deepEqual(parse({ t: 'poke', kind: 'pickup' }), { t: 'poke', kind: 'pickup' });
  assert.deepEqual(parse({ t: 'poke', kind: 'squeeze' }), { t: 'poke', kind: 'other' }, 'newer gestures never drop the link');
  assert.deepEqual(parse({ t: 'ping', ts: 123 }), { t: 'ping', ts: 123 });
  assert.deepEqual(parseDeviceMessage(Buffer.from('{"t":"ping"}')), { t: 'ping', ts: undefined });
});

test('invalid device frames throw ProtocolError', () => {
  const bad = [
    'not json', '[]', 'null', '42', { t: 'nope' }, {},
    { t: 'hello', v: 5, device: 'a', token: 'b' }, { t: 'hello', v: 2, device: 'a', token: 'b' }, { t: 'hello', v: 1, device: '', token: 'b' },
    { t: 'hello', v: 1, device: 'a' }, { t: 'hello', v: 1, device: 'a'.repeat(65), token: 'b' },
    { t: 'ptt', on: 'yes', turn: 1 }, { t: 'ptt', on: true, turn: 256 }, { t: 'ptt', on: true, turn: -1 }, { t: 'ptt', on: true },
    { t: 'ptt', on: true, turn: 1, ms: -5 }, { t: 'cancel', gen: 1.5 }, { t: 'played' }, { t: 'played', gen: 7 }, { t: 'played', gen: 300 },
    { t: 'poke', kind: 'Kick!' }, { t: 'poke', kind: 7 }, { t: 'ping', ts: 'x' },
  ];
  for (const frame of bad) assert.throws(() => parse(frame), ProtocolError, JSON.stringify(frame));
  assert.throws(() => parse(JSON.stringify({ t: 'ping', pad: 'x'.repeat(4096) })), /4 KiB/);
});

test('v5 hello, auth and device keys', () => {
  const id = deviceKey('kubik-1');
  assert.deepEqual(parse({ ...id.hello(), extra: 1 }), { t: 'hello', v: 5, device: 'kubik-1', key: id.key, fw: '0.6.1', name: 'Кубик', volume: 100 });
  assert.deepEqual(parse({ ...id.hello(), volume: 0 }), { t: 'hello', v: 5, device: 'kubik-1', key: id.key, fw: '0.6.1', name: 'Кубик', volume: 0 });
  assert.deepEqual(parse({ t: 'hello', v: 5, device: 'kubik-1', key: id.key }), { t: 'hello', v: 5, device: 'kubik-1', key: id.key, fw: undefined, name: undefined });
  assert.deepEqual(parse({ t: 'auth', sig: 'MEUCIQ==' }), { t: 'auth', sig: 'MEUCIQ==' });
  const raw = Buffer.from(id.key, 'base64');
  const offCurve = Buffer.from(raw); offCurve[64] ^= 1;
  const compressed = Buffer.concat([Buffer.from([2]), raw.subarray(1, 33)]);
  const bad = [
    { t: 'hello', v: 5, device: 'kubik-1' }, { t: 'hello', v: 5, device: '', key: id.key }, { t: 'hello', v: 5, device: 'a'.repeat(65), key: id.key },
    { t: 'hello', v: 5, device: 'kubik-1', key: offCurve.toString('base64') },
    { t: 'hello', v: 5, device: 'kubik-1', key: compressed.toString('base64') },
    { t: 'hello', v: 5, device: 'kubik-1', key: Buffer.concat([raw, Buffer.from([0])]).toString('base64') },
    { t: 'hello', v: 5, device: 'kubik-1', key: raw.toString('base64url') },
    { t: 'hello', v: 5, device: 'kubik-1', key: id.key.replace(/=+$/, '') },
    { t: 'hello', v: 5, device: 'kubik-1', key: 42 },
    { t: 'hello', v: 3, device: 'kubik-1', key: id.key }, // v3 is gone: its signature has no transport binding
    { ...id.hello(), name: 'Кубик\u202e' }, { ...id.hello(), name: 'a\nb' }, { ...id.hello(), name: '   ' },
    { ...id.hello(), name: 'x'.repeat(33) }, { ...id.hello(), fw: '0.5.0 beta' }, { ...id.hello(), fw: '0.5.\u0000' },
    { ...id.hello(), fw: '1'.repeat(33) },
    { t: 'auth' }, { t: 'auth', sig: '' }, { t: 'auth', sig: 'not base64!' }, { t: 'auth', sig: 'A'.repeat(400) },
  ];
  for (const frame of bad) assert.throws(() => parse(frame), ProtocolError, JSON.stringify(frame));
  const decoded = decodeDeviceKey(id.key);
  assert.equal(decoded.fingerprint, id.fingerprint);
  assert.match(decoded.fingerprint, /^[0-9a-f]{32}$/);
  const nonce = 'bm9uY2U=';
  const message = authMessage({ nonce, device: 'kubik-1', key: id.key, bind: caBind('example.com') });
  assert.equal(message.toString('utf8'), `kubik-auth-v5\n${nonce}\nkubik-1\n${id.key}\nca:example.com`);
  assert.equal(verify('sha256', message, { key: decoded.publicKey, dsaEncoding: 'der' }, Buffer.from(id.sign(nonce, { bind: 'ca:example.com' }), 'base64')), true);
  assert.equal(CLOSE.PAIRING_TIMEOUT, 4004);
  assert.equal(CLOSE.PAIRING_BUSY, 4005);
});

test('volume state and per-device agent model picker frames are bounded', () => {
  assert.deepEqual(parse({ t: 'device_state', volume: 0 }), { t: 'device_state', volume: 0 });
  assert.deepEqual(parse({ t: 'device_state', volume: 100 }), { t: 'device_state', volume: 100 });
  assert.deepEqual(parse({ t: 'agent_options', target: 'agent', rid: 65535, cursor: 255 }), { t: 'agent_options', target: 'agent', rid: 65535, cursor: 255 });
  assert.deepEqual(parse({ t: 'agent_options', target: 'agent', rid: 0 }), { t: 'agent_options', target: 'agent', rid: 0, cursor: 0 });
  assert.deepEqual(parse({ t: 'agent_model', target: 'agent', rid: 7, cursor: 1, id: 'openrouter/meta-llama/model' }),
    { t: 'agent_model', target: 'agent', rid: 7, cursor: 1, id: 'openrouter/meta-llama/model' });
  for (const frame of [
    { t: 'device_state', volume: -1 }, { t: 'device_state', volume: 101 }, { t: 'device_state', volume: 1.5 },
    { t: 'agent_options', target: 'agent', rid: -1 }, { t: 'agent_options', target: 'agent', rid: 65536 }, { t: 'agent_options', target: 'agent', rid: 1.5 },
    { t: 'agent_options', target: 'agent', rid: 1, cursor: 256 }, { t: 'agent_model', target: 'agent', rid: 1, cursor: -1, id: 'provider/model' },
    { t: 'agent_model', target: 'agent', rid: 1, cursor: 256, id: 'provider/model' }, { t: 'agent_model', target: 'agent', rid: 1, id: 'provider/model' },
    { t: 'agent_model', target: 'agent', rid: 1, cursor: 0, id: '' },
    { t: 'agent_model', target: 'agent', rid: 1, cursor: 0, id: 'x'.repeat(161) }, { t: 'agent_model', target: 'agent', rid: 1, cursor: 0, id: 'provider/model\n' },
    { t: 'prefs' },
  ]) assert.throws(() => parse(frame), ProtocolError);
});

test('microphone IMA frames: independent state, kind, size and invalid index', () => {
  const encoder = new ImaEncoder(), pcm = Buffer.alloc(1920);
  for (let i = 0; i < 960; i++) pcm.writeInt16LE(Math.round(8000 * Math.sin(i * .07)), i * 2);
  const first = Buffer.concat([Buffer.from([4, 9]), encoder.encode(pcm)]);
  for (let i = 0; i < 960; i++) pcm.writeInt16LE(Math.round(8000 * Math.sin((i + 960) * .07)), i * 2);
  const second = Buffer.concat([Buffer.from([4, 9]), encoder.encode(pcm)]);
  const parsed = parseAudioFrame(second); // Does not depend on receiving the first packet.
  assert.equal(first.length, 485);
  assert.equal(parsed.turn, 9); assert.equal(parsed.pcm.length, 1920);
  let error = 0, power = 0;
  for (let i = 0; i < 960; i++) {
    const sample = pcm.readInt16LE(i * 2), delta = parsed.pcm.readInt16LE(i * 2) - sample;
    error += delta * delta; power += sample * sample;
  }
  assert.ok(10 * Math.log10(power / error) > 25);
  assert.throws(() => parseAudioFrame(Buffer.from([1, 1, 0, 0, 0, 0])), /kind/);
  assert.throws(() => parseAudioFrame(Buffer.from([4, 1, 0, 0, 0])), /short/);
  assert.throws(() => parseAudioFrame(Buffer.from([4, 1, 0, 0, 89, 0])), /index/);
  assert.throws(() => parseAudioFrame(Buffer.alloc(486, 4)), /40 ms/);
});

test('IMA ADPCM speech: a quarter of the bytes, continuous across frames, close to the source', () => {
  const pcm = Buffer.alloc(4800 * 3);
  for (let i = 0; i < pcm.length / 2; i++) {
    pcm.writeInt16LE(Math.round(8000 * Math.sin(i * 0.07) + 3000 * Math.sin(i * 0.31)), i * 2);
  }
  const encoder = new ImaEncoder();
  const decoded = [];
  let prev = null;
  for (let off = 0; off < pcm.length; off += 4800) {
    const payload = encoder.encode(pcm.subarray(off, off + 4800));
    assert.equal(payload.length, 3 + 1200);
    const frame = speechFrameIma(9, payload);
    assert.deepEqual([frame[0], frame[1]], [3, 9]);
    // Each frame carries the state the previous one ended in, so the device can start from any of them.
    if (prev) assert.equal(payload.readInt16LE(0), prev.readInt16LE(prev.length - 2));
    prev = imaDecode(payload);
    decoded.push(prev);
  }
  const out16 = Buffer.concat(decoded);
  let signal = 0, noise = 0;
  for (let i = 200; i < pcm.length / 2; i++) {
    const s = pcm.readInt16LE(i * 2);
    signal += s * s;
    noise += (s - out16.readInt16LE(i * 2)) ** 2;
  }
  assert.ok(10 * Math.log10(signal / noise) > 25, 'SNR over 25 dB');
  // Bit-exact with the firmware decoder's encoder (firmware/sim/ima_adpcm_test.c prints the same hash).
  const ref = Buffer.alloc(48000);
  for (let i = 0; i < 24000; i++) ref.writeInt16LE(Math.round(8000 * Math.sin(i * 0.07) + 3000 * Math.sin(i * 0.31)), i * 2);
  const refFrame = new ImaEncoder().encode(ref);
  let hash = 2166136261;
  for (let i = 3; i < refFrame.length; i++) hash = Math.imul(hash ^ refFrame[i], 16777619) >>> 0;
  assert.equal(hash, 0x14f87442);
  // An odd sample count is padded to a whole byte.
  assert.equal(new ImaEncoder().encode(Buffer.alloc(6)).length, 3 + 2);
});

test('IMA speech frames and outgoing builders', () => {
  const frame = speechFrameIma(4, new ImaEncoder().encode(Buffer.alloc(4800, 7)));
  assert.equal(frame[0], 3);
  assert.equal(frame[1], 4);
  assert.ok(frame.length < 4802);
  assert.deepEqual(out.state('thinking'), { t: 'state', s: 'thinking' });
  assert.deepEqual(out.emotion('happy'), { t: 'emotion', e: 'happy' });
  assert.deepEqual(out.speak(1, 'notify'), { t: 'speak', gen: 1, kind: 'notify' });
  assert.deepEqual(out.error('stt_empty'), { t: 'error', code: 'stt_empty' });
  assert.deepEqual(out.pong(5), { t: 'pong', ts: 5 });
  assert.deepEqual(out.challenge('bm9uY2U='), { t: 'challenge', nonce: 'bm9uY2U=' });
  assert.deepEqual(out.pair('ABCD2345'), { t: 'pair', code: 'ABCD2345' });
  assert.deepEqual(out.pair(''), { t: 'pair', code: '' });
  for (const message of [out.welcome('s-1', 70), out.speakEnd(3)]) assert.ok(JSON.stringify(message).length < 4096);
});

test('gen wraps 1..255 and never uses 0', () => {
  assert.equal(nextGen(0), 1);
  assert.equal(nextGen(1), 2);
  assert.equal(nextGen(254), 255);
  assert.equal(nextGen(255), 1);
});

test('SpeechPacer bounds unconfirmed audio, frames <= 4800 bytes, markers in order', async () => {
  let clock = 0;
  const sent = [];
  const events = [];
  let confirmed = 0;
  const pacer = new SpeechPacer({ progressWindowMs: 600, now: () => clock, sendAudio: (pcm) => { sent.push({ at: clock, confirmed, bytes: pcm.length }); events.push('a'); } });
  // The device starts after prebuffering and reports actual consumption.
  const ticker = setInterval(() => { clock += 50; confirmed = Math.min(pacer.sentMs, Math.max(0, clock - 400)); pacer.played(confirmed); }, 1);
  pacer.push(Buffer.alloc(48 * 2000)); // 2 s
  pacer.mark((playAt) => events.push(`m@${Math.round(playAt)}`));
  pacer.push(Buffer.alloc(48 * 500));
  await pacer.drained();
  clearInterval(ticker);
  assert.ok(sent.every((f) => f.bytes <= 4800 && f.bytes % 2 === 0));
  assert.equal(sent.reduce((s, f) => s + f.bytes, 0), 48 * 2500);
  // Time passing never grants credit; only confirmed consumption does.
  let total = 0;
  for (const f of sent) { total += f.bytes; assert.ok(total / 48 - f.confirmed <= 600, `unconfirmed ${total / 48 - f.confirmed}`); }
  const markerIndex = events.findIndex((e) => e.startsWith('m@'));
  assert.equal(events.slice(0, markerIndex).length, 2000 / 100);
  assert.ok(Number(events[markerIndex].slice(2)) >= 2000 - 1, 'marker play time is after the first 2 s of audio');
  assert.equal(pacer.sentMs, 2500);
  pacer.startGen();
  assert.equal(pacer.sentMs, 0);
});

test('fresh consumption after delayed ACKs immediately releases credit despite the stale time estimate', async (t) => {
  let clock = 0, sent = 0;
  const pacer = new SpeechPacer({ progressWindowMs: 1200, now: () => clock, sendAudio: pcm => { sent += pcm.length / 48; } });
  t.after(() => pacer.clear());
  const settle = () => new Promise(resolve => setImmediate(resolve));
  pacer.startGen(); pacer.push(Buffer.alloc(3000 * 48));
  assert.equal(sent, 1200);
  clock = 1000; pacer.played(100); await settle();
  assert.equal(sent, 1300);
  // A delayed old report followed by a fresh one: the device now has 900 ms more room.
  // The former wall-clock limiter withheld all 900 ms here and could empty its ring.
  pacer.played(1000); await settle();
  assert.equal(sent, 2200);
  assert.equal(sent - 1000, 1200, 'strict capacity still includes in-flight audio');
  pacer.clear(); await pacer.drained();
});

test('SpeechPacer.clear drops queued audio immediately', async () => {
  const sent = [];
  const pacer = new SpeechPacer({ progressWindowMs: 100, sendAudio: (pcm) => sent.push(pcm.length) });
  pacer.push(Buffer.alloc(48 * 5000));
  await new Promise((r) => setTimeout(r, 50));
  pacer.clear();
  await pacer.drained();
  const bytes = sent.reduce((a, b) => a + b, 0);
  assert.ok(bytes < 48 * 1000, `sent ${bytes}`);
  assert.equal(pacer.queuedBytes, 0);
});

test('slow initial deltas followed by a burst preserve prebuffer and ring capacity', async () => {
  let clock = 0;
  const sent = [];
  const pacer = new SpeechPacer({ progressWindowMs: 1000, prebufMs: 400, now: () => clock, sendAudio: (pcm) => sent.push({ at: clock, ms: pcm.length / 48 }) });
  pacer.startGen();
  // A streaming voice model: 300 ms of audio trickles in over the first second, then 4 s arrive at once.
  for (let i = 0; i < 3; i++) { pacer.push(Buffer.alloc(48 * 100)); clock += 330; await new Promise((r) => setImmediate(r)); }
  const playbackStart = clock;
  const ticker = setInterval(() => { clock += 20; pacer.played(Math.min(pacer.sentMs, clock - playbackStart)); }, 1);
  pacer.push(Buffer.alloc(48 * 4000));
  await pacer.drained();
  clearInterval(ticker);
  // Device model (firmware audio.c): nothing plays until 400 ms are buffered, then real time.
  let held = 0, startedAt = null, last = 0, max = 0;
  for (const f of sent) {
    if (startedAt !== null) held = Math.max(0, held - (f.at - last));
    last = f.at;
    held += f.ms;
    if (startedAt === null && held >= 400) startedAt = f.at;
    max = Math.max(max, held);
  }
  assert.ok(max <= 1200, `device buffer peaked at ${max} ms`);
});

test('progress window includes in-flight network audio and a stalled receiver closes within five seconds', async () => {
  let clock = 0, sent = 0, stalled = 0;
  const pacer = new SpeechPacer({ progressWindowMs: 1200, now: () => clock,
    sendAudio: pcm => { sent += pcm.length / 48; }, onStall: () => { stalled++; } });
  pacer.startGen(); pacer.push(Buffer.alloc(3000 * 48));
  assert.equal(sent, 1200);
  clock = 1000; await new Promise(resolve => setTimeout(resolve, 30));
  assert.equal(sent, 1200, 'network delay must not be mistaken for speaker consumption');
  pacer.played(400); await new Promise(resolve => setTimeout(resolve, 30));
  assert.equal(sent, 1600);
  pacer.played(300); // out-of-order progress cannot release additional credit
  clock = 4000; pacer.played(400); // repeated reports are not forward progress
  clock = 6100; await pacer.drained();
  assert.equal(sent, 1600); assert.equal(stalled, 1);
});
