import assert from 'node:assert/strict';
import test from 'node:test';
import { EchoCleaner } from '../src/echo-cleaner.js';
import { ImaEncoder, parseAudioFrame } from '../src/protocol.js';

function packet(mic, ref) {
  return parseAudioFrame(Buffer.concat([Buffer.from([5, 3]), mic, ref]));
}
const energy = pcm => { let sum = 0; for (let i = 0; i < pcm.length; i += 2) sum += pcm.readInt16LE(i) ** 2; return sum; };

test('Live synchronized two-channel framing rejects malformed input and decodes each channel independently', () => {
  const mic = new ImaEncoder(), ref = new ImaEncoder();
  const near = Buffer.alloc(1920), far = Buffer.alloc(1920);
  for (let i = 0; i < 960; i++) { near.writeInt16LE(1000, i * 2); far.writeInt16LE(-1000, i * 2); }
  const result = packet(mic.encode(near), ref.encode(far));
  assert.equal(result.turn, 3); assert.equal(result.pcm.length, 1920); assert.equal(result.reference.length, 1920);
  assert.ok(result.pcm.readInt16LE(1900) > 900); assert.ok(result.reference.readInt16LE(1900) < -900);
  assert.throws(() => parseAudioFrame(Buffer.alloc(968)), /kind/);
  assert.throws(() => parseAudioFrame(Buffer.concat([Buffer.from([5, 3]), Buffer.alloc(964)])), /two 40 ms/);
  const bad = Buffer.concat([Buffer.from([5, 3]), Buffer.alloc(966)]); bad[487] = 89;
  assert.throws(() => parseAudioFrame(bad), /step index/);
});

function acousticCase(delay, gain) {
  const cleaner = new EchoCleaner();
  const micEncoder = new ImaEncoder(), refEncoder = new ImaEncoder();
  let seed = 12345, low = 0;
  const history = new Int16Array(24000 * 12);
  let rawEnergy = 0, cleanEnergy = 0, nearEnergy = 0, nearCorrelation = 0, cleanedNearEnergy = 0;
  let processingMs = 0;
  for (let frame = 0; frame < 300; frame++) {
    const mic = Buffer.alloc(1920), reference = Buffer.alloc(1920), near = new Int16Array(960);
    for (let i = 0; i < 960; i++) {
      const at = frame * 960 + i;
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      low = .8 * low + .2 * ((seed / 0xffffffff - .5) * 16000);
      history[at] = low * gain;
      const echo = .45 * (history[at - delay] ?? 0) + .15 * (history[at - delay - 480] ?? 0);
      near[i] = frame >= 200 ? Math.round(2200 * (.45 + .55 * Math.sin(at / 2400) ** 2) *
        (Math.sin(at * 2 * Math.PI * 233 / 24000) + .65 * Math.sin(at * 2 * Math.PI * 349 / 24000) + .3 * Math.sin(at * 2 * Math.PI * 701 / 24000))) : 0;
      mic.writeInt16LE(Math.round(echo + near[i]), i * 2); reference.writeInt16LE(history[at], i * 2);
    }
    const decoded = packet(micEncoder.encode(mic), refEncoder.encode(reference));
    const start = performance.now(), cleaned = cleaner.process(decoded.pcm, decoded.reference);
    processingMs += performance.now() - start;
    if (frame >= 100 && frame < 190) { rawEnergy += energy(decoded.pcm); cleanEnergy += energy(cleaned); }
    if (frame >= 220) for (let i = 0; i < 960; i++) {
      nearEnergy += near[i] ** 2; nearCorrelation += near[i] * cleaned.readInt16LE(i * 2);
      cleanedNearEnergy += cleaned.readInt16LE(i * 2) ** 2;
    }
  }
  const erle = 10 * Math.log10(rawEnergy / cleanEnergy);
  const similarity = nearCorrelation / Math.sqrt(nearEnergy * cleanedNearEnergy);
  assert.ok(erle > 15, `Echo reduction too small: ${erle.toFixed(1)} dB`);
  assert.ok(similarity > .9, `Near speech lost during double-talk: ${similarity.toFixed(3)}`);
  assert.ok(cleanedNearEnergy / nearEnergy > .7, 'Canceller muted near speech');
  assert.ok(processingMs < 600, 'AEC cannot process 12 seconds of audio with sufficient CPU margin');
  cleaner.close();
  console.log(`AEC: delay ${delay} gain ${gain}, IMA echo ${erle.toFixed(1)} dB, double-talk correlation ${similarity.toFixed(3)}, CPU ${processingMs.toFixed(1)} ms / 12 s`);
}

test('Persistent Speex AEC preserves modulated near speech across echo delays and levels after IMA decoding', () => {
  for (const [delay, gain] of [[60, .5], [600, 1], [1800, 2]]) acousticCase(delay, gain);
});

test('AEC rejects missing/skewed channels, releases idempotently and preserves near input without a reference signal', () => {
  const cleaner = new EchoCleaner(), near = Buffer.alloc(1920);
  for (let i = 0; i < 960; i++) near.writeInt16LE(Math.round(2500 * Math.sin(i / 6)), i * 2);
  assert.throws(() => cleaner.process(near));
  assert.throws(() => cleaner.process(near, Buffer.alloc(960)));
  const output = cleaner.process(near, Buffer.alloc(1920));
  assert.ok(energy(output) / energy(near) > .7);
  cleaner.close(); cleaner.close(); assert.throws(() => cleaner.process(near, near));
});

test('Persistent near-only speech-like input remains audible through six seconds of adaptation', t => {
  const cleaner = new EchoCleaner(); t.after(() => cleaner.close());
  const encoder = new ImaEncoder(), reference = new ImaEncoder();
  let seed = 73, previous = 0, nearEnergy = 0, cleanEnergy = 0, dot = 0;
  for (let frame = 0; frame < 150; frame++) {
    const near = Buffer.alloc(1920);
    for (let i = 0; i < 960; i++) {
      const at = frame * 960 + i;
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      previous = .65 * previous + .35 * ((seed / 0xffffffff - .5) * 6000);
      const syllable = .15 + .85 * Math.sin(at / 2400) ** 2;
      near.writeInt16LE(Math.round(syllable * (previous + 1500 * Math.sin(at * Math.PI * 260 / 24000))), i * 2);
    }
    const decoded = packet(encoder.encode(near), reference.encode(Buffer.alloc(1920)));
    const output = cleaner.process(decoded.pcm, decoded.reference);
    for (let i = 0; i < 960; i++) {
      const a = decoded.pcm.readInt16LE(i * 2), b = output.readInt16LE(i * 2);
      nearEnergy += a * a; cleanEnergy += b * b; dot += a * b;
    }
  }
  assert.ok(cleanEnergy / nearEnergy > .7, 'AEC attenuated a standalone user');
  assert.ok(dot / Math.sqrt(cleanEnergy * nearEnergy) > .9, 'AEC distorted standalone speech-like input');
});
