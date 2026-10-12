import assert from 'node:assert/strict';
import test from 'node:test';
import { createHash } from 'node:crypto';
import { DeviceSession } from '../src/session.js';
import { ImaEncoder, speechFrameIma } from '../src/protocol.js';

function pcm(samples, seed = 1) {
  const data = Buffer.alloc(samples * 2);
  for (let i = 0; i < samples; i++) data.writeInt16LE(((i * 7919 + seed * 1237) % 65536) - 32768, i * 2);
  return data;
}

test('direct speech encoding preserves wire bytes, state and owned buffers', () => {
  const direct = new ImaEncoder(), reference = new ImaEncoder();
  const retained = [];
  for (const [i, samples] of [0, 1, 2, 3, 959, 960, 2399, 2400, 2400].entries()) {
    const input = pcm(samples, i), original = Buffer.from(input);
    const gen = i % 2 ? 255 : 1;
    const expected = speechFrameIma(gen, reference.encode(input));
    const actual = direct.encodeSpeech(gen, input);
    assert.deepEqual(actual, expected);
    assert.deepEqual(input, original);
    assert.equal(direct.pred, reference.pred);
    assert.equal(direct.index, reference.index);
    retained.push({ actual, expected });
  }
  // Queued WebSocket sends may still own earlier frames when encoding resumes.
  for (const { actual, expected } of retained) assert.deepEqual(actual, expected);
  // Frozen from main 74a4aa3, independent of the shared encoder implementation.
  const hash = createHash('sha256');
  for (const { actual } of retained) hash.update(actual);
  assert.equal(hash.digest('hex'), 'ae158e27e0b83430baaab00c70d0405987da838dcb709a9c2239893d9d6d7d96');
  assert.deepEqual(direct.encode(pcm(31)), reference.encode(pcm(31)), 'payload API shares the same state');
});

test('session encodes assembled PCM with one wire allocation and resets state on generation changes', t => {
  const sent = [];
  const session = new DeviceSession({ ws: { readyState: 1, send: data => sent.push(data) },
    device: { id: 'kubik-test' }, engine: {}, dispatch: async () => {} });
  t.after(() => session.close());
  const input = pcm(2400);
  const reference = new ImaEncoder();
  const expected = [speechFrameIma(255, reference.encode(input)), speechFrameIma(255, reference.encode(input)),
    speechFrameIma(1, new ImaEncoder().encode(input))];
  const allocate = Buffer.allocUnsafe, copy = Buffer.prototype.copy;
  const sizes = [];
  let copies = 0;
  t.mock.method(Buffer, 'allocUnsafe', size => { sizes.push(size); return allocate(size); });
  t.mock.method(Buffer.prototype, 'copy', function (...args) { copies++; return copy.apply(this, args); });
  session.activeGen = 255;
  session.pacer.startGen();
  session.pacer.push(input); session.pacer.push(input);
  session.activeGen = 1;
  session.pacer.startGen(); session.pacer.push(input);
  t.mock.restoreAll();
  assert.deepEqual(sent, expected);
  assert.deepEqual(sizes, [1205, 1205, 1205]);
  assert.equal(copies, 0, 'speech payload must be encoded directly into its wire frame');
});
