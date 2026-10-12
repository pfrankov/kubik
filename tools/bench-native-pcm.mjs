// Opt-in experiment; the alternatives below are never imported by the host.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { cpus } from 'node:os';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { SpeechStream } from '../openclaw-kubik/src/speech-stream.js';
import { SpeechPacer } from '../openclaw-kubik/src/audio.js';
import { ImaEncoder } from '../openclaw-kubik/src/protocol.js';

const ROOT = fileURLToPath(new URL('../', import.meta.url));
const BASE = '74a4aa33f17652947ae56586533753639f1ddcf8';
const source = execFileSync('git', ['show', `${BASE}:openclaw-kubik/src/speech-stream.js`], { cwd: ROOT, encoding: 'utf8' });
const traceUrl = new URL('../openclaw-kubik/src/speech-trace.js', import.meta.url).href;
const frozen = source.replace("'./speech-trace.js'", JSON.stringify(traceUrl));
const { SpeechStream: Baseline } = await import(`data:text/javascript;base64,${Buffer.from(frozen).toString('base64')}`);
const FRAME_BYTES = 400 * 48;

class Growing extends Baseline {
  constructor(fixed, unpooled) {
    super('reply', 1);
    this.fixed = fixed; this.unpooled = unpooled;
  }
  allocate(size) { return this.unpooled ? Buffer.allocUnsafeSlow(size) : Buffer.allocUnsafe(size); }
  seal(item) {
    if (!item?.storage) return;
    if (item.pcm.length < item.storage.length) {
      const out = this.allocate(item.pcm.length);
      item.pcm.copy(out); item.pcm = out;
    }
    delete item.storage;
  }
  take() { const item = super.take(); this.seal(item); return item; }
  async enqueue(item) { this.seal(this.items.at(-1)); return super.enqueue(item); }
  enqueuePcm(pcm, frameBytes) {
    let at = 0;
    const tail = this.items.at(-1);
    if (tail?.pcm && tail.pcm.length < frameBytes) {
      at = Math.min(pcm.length, frameBytes - tail.pcm.length);
      const size = tail.pcm.length + at;
      let storage = tail.storage ?? tail.pcm;
      if (storage.length < size) {
        const capacity = this.fixed ? frameBytes : Math.min(frameBytes, Math.max(size, tail.pcm.length * 2));
        storage = this.allocate(capacity);
        tail.pcm.copy(storage); tail.storage = storage;
      }
      pcm.copy(storage, tail.pcm.length, 0, at);
      tail.pcm = storage.subarray(0, size);
    }
    for (; at < pcm.length; at += frameBytes) this.items.push({ pcm: Buffer.from(pcm.subarray(at, at + frameBytes)) });
  }
}

const versions = {
  baseline: () => new Baseline('reply', 1), current: () => new SpeechStream('reply', 1),
  geometric: () => new Growing(false, false), fixed: () => new Growing(true, false),
  geometricUnpooled: () => new Growing(false, true), fixedUnpooled: () => new Growing(true, true),
};

function retained(items) {
  const buffers = new Set(); let logicalBytes = 0;
  for (const item of items) {
    logicalBytes += item.pcm?.length ?? 0;
    if (item.pcm) buffers.add(item.pcm.buffer);
    if (item.storage) buffers.add(item.storage.buffer);
  }
  return { logicalBytes, backingBytes: [...buffers].reduce((n, buffer) => n + buffer.byteLength, 0), items: items.length };
}

function drain(stream, views) {
  while (stream.items.length) {
    const pcm = stream.take().pcm;
    // Model a retained remainder, as SpeechPacer.#takeFrame does. This is not a live pacer or RSS measurement.
    views.push({ pcm: pcm.subarray(Math.min(4800, Math.floor(pcm.length / 2))) });
  }
}

function episode(make, input, flushEvery, durationMs, measured) {
  const stream = make(), views = [];
  const chunks = Math.ceil(durationMs * 48 / input.length);
  for (let i = 0; i < chunks; i++) {
    stream.enqueuePcm(input, FRAME_BYTES);
    if (measured) measured.queuePeakBackingBytes = Math.max(measured.queuePeakBackingBytes, retained(stream.items).backingBytes);
    if ((i + 1) % flushEvery === 0) drain(stream, views);
  }
  if (measured) {
    measured.queued = retained(stream.items);
    measured.beforeTake = retained([...stream.items, ...views]);
  }
  drain(stream, views);
  if (measured) measured.downstreamViews = retained(views);
  stream.cancel();
  return views.length;
}

function counters(make, input, flushEvery) {
  const original = { from: Buffer.from, concat: Buffer.concat, allocUnsafe: Buffer.allocUnsafe,
    allocUnsafeSlow: Buffer.allocUnsafeSlow, copy: Buffer.prototype.copy, subarray: Buffer.prototype.subarray };
  const out = { allocations: 0, requestedBytes: 0, copiedBytes: 0, viewObjects: 0, queuePeakBackingBytes: 0 };
  Buffer.from = function (...args) {
    const result = original.from(...args);
    if (Buffer.isBuffer(args[0])) { out.allocations++; out.requestedBytes += result.length; out.copiedBytes += result.length; }
    return result;
  };
  Buffer.concat = function (...args) {
    const result = original.concat(...args);
    out.allocations++; out.requestedBytes += result.length; out.copiedBytes += result.length;
    return result;
  };
  for (const name of ['allocUnsafe', 'allocUnsafeSlow']) {
    Buffer[name] = function (size) { out.allocations++; out.requestedBytes += size; return original[name](size); };
  }
  Buffer.prototype.copy = function (...args) { const bytes = original.copy.apply(this, args); out.copiedBytes += bytes; return bytes; };
  Buffer.prototype.subarray = function (...args) { out.viewObjects++; return original.subarray.apply(this, args); };
  try { episode(make, input, flushEvery, 60000, out); }
  finally {
    for (const name of ['from', 'concat', 'allocUnsafe', 'allocUnsafeSlow']) Buffer[name] = original[name];
    Buffer.prototype.copy = original.copy; Buffer.prototype.subarray = original.subarray;
  }
  return out;
}

function median(values) { return [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)]; }
function timing(make, input, flushEvery) {
  for (let i = 0; i < 8; i++) episode(make, input, flushEvery, 2400);
  const chunks = Math.ceil(2400 * 48 / input.length);
  const rounds = Math.max(8, Math.min(400, Math.floor(10000 / chunks)));
  const wall = [], cpu = [];
  for (let sample = 0; sample < 7; sample++) {
    global.gc();
    const started = performance.now(), usage = process.cpuUsage();
    for (let round = 0; round < rounds; round++) episode(make, input, flushEvery, 2400);
    const spent = process.cpuUsage(usage);
    wall.push((performance.now() - started) / rounds); cpu.push((spent.user + spent.system) / 1000 / rounds);
  }
  return { cpuMsPer2400ms: median(cpu), wallMsPer2400ms: median(wall), wallRangeMs: [Math.min(...wall), Math.max(...wall)], rounds };
}

function assembled(make, input, deltaBytes) {
  const stream = make();
  for (let at = 0; at < input.length; at += deltaBytes) stream.enqueuePcm(input.subarray(at, at + deltaBytes), FRAME_BYTES);
  const items = [];
  while (stream.items.length) items.push(stream.take());
  assert.equal(items.length, Math.ceil(input.length / FRAME_BYTES));
  for (const item of items) assert.deepEqual(Object.keys(item), ['pcm']);
  assert.deepEqual(Buffer.concat(items.map(item => item.pcm)), input);
  return items;
}

async function wire(items) {
  const packets = [], encoder = new ImaEncoder();
  const pacer = new SpeechPacer({ sendAudio: pcm => {
    packets.push(encoder.encodeSpeech(7, pcm));
    queueMicrotask(() => pacer.played(pacer.sentMs));
  } });
  pacer.startGen();
  for (const item of items) pacer.push(item.pcm);
  pacer.mark(() => packets.push('end'));
  await pacer.drained();
  assert.equal(pacer.queuedBytes, 0);
  return packets;
}

async function boundaries(make) {
  const stream = make(), source = Buffer.alloc(960, 31), expected = Buffer.from(source);
  stream.enqueuePcm(source, FRAME_BYTES); source.fill(63);
  stream.enqueuePcm(source, FRAME_BYTES);
  const text = { text: 'marker' }; await stream.enqueue(text);
  stream.enqueuePcm(source, FRAME_BYTES);
  const first = stream.take().pcm;
  assert.deepEqual(first, Buffer.concat([expected, source]));
  assert.equal(stream.take(), text);
  stream.enqueuePcm(source, FRAME_BYTES);
  stream.items.length = 0; // The host's failed-notification path also discards items directly.
  stream.enqueuePcm(source, FRAME_BYTES);
  assert.deepEqual(stream.take().pcm, source);
  stream.enqueuePcm(source, FRAME_BYTES); stream.enqueuePcm(source, FRAME_BYTES);
  stream.cancel(); assert.equal(stream.items.length, 0);
  assert.deepEqual(first, Buffer.concat([expected, source]));
  const next = make(); next.enqueuePcm(source, FRAME_BYTES);
  assert.deepEqual(next.take().pcm, source);
}

async function verify() {
  const input = Buffer.alloc(2400 * 48 + 38);
  for (let i = 0; i < input.length; i++) input[i] = i % 251;
  const expected = await wire(assembled(versions.baseline, input, 960));
  for (const make of Object.values(versions)) {
    for (const size of [48, 960, 4800, 96000]) assert.deepEqual(await wire(assembled(make, input, size)), expected);
    await boundaries(make);
  }
  // The instrumentation must recover the analytic baseline, not double count Buffer internals.
  const tiny = counters(versions.baseline, Buffer.alloc(48), Infinity);
  assert.equal(tiny.allocations, 60000); assert.equal(tiny.copiedBytes, 577440000);
  const immediate = counters(versions.baseline, Buffer.alloc(960), 1);
  assert.equal(immediate.allocations, 3000); assert.equal(immediate.copiedBytes, 2880000);
}

assert.ok(process.argv.slice(2).every(arg => arg === '--verify-only'), 'only --verify-only is supported');
await verify();
const digest = value => createHash('sha256').update(value).digest('hex');
console.log(JSON.stringify({ verified: true, baseline: BASE, baselineSourceSha256: digest(source),
  currentSourceSha256: digest(readFileSync(new URL('../openclaw-kubik/src/speech-stream.js', import.meta.url))),
  benchmarkSourceSha256: digest(readFileSync(fileURLToPath(import.meta.url))),
  current: execFileSync('git', ['rev-parse', 'HEAD'], { cwd: ROOT, encoding: 'utf8' }).trim(),
  node: process.version, platform: process.platform, arch: process.arch, cpu: cpus()[0]?.model, poolSize: Buffer.poolSize }));
if (!process.argv.includes('--verify-only')) {
  assert.equal(typeof global.gc, 'function', 'run with node --expose-gc');
  const modes = { queued: Infinity, immediate: 1, partial3: 3, partial5: 5 };
  for (const [mode, flushEvery] of Object.entries(modes)) {
    for (const deltaMs of [1, 20, 40, 100, 400]) {
      const input = Buffer.alloc(deltaMs * 48, 31);
      for (const [name, make] of Object.entries(versions)) {
        console.log(JSON.stringify({ mode, deltaMs, name, ...timing(make, input, flushEvery), ...counters(make, input, flushEvery) }));
      }
    }
  }
}
