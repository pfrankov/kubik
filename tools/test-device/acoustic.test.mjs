import test from 'node:test';
import assert from 'node:assert/strict';
import { inspectTurn, inspectCapture } from './acoustic.mjs';

const input = 'voice: input frames=25 pcm=1000ms elapsed=1000ms send=25ms max=5ms over40=0 dropped=0ms\n';
const played = 'app: speech begin gen=4\napp: speech played gen=4 ms=1000 elapsed=1060 gaps=0 (0 ms) mix<=10us dma-wait<=20us via=wifi';
test('acoustic acceptance rejects truncated replies even when played is present', () => {
  assert.equal(inspectTurn(input), null);
  assert.equal(inspectTurn(input + played).speechMs, 1000);
  assert.throws(() => inspectTurn(input + played, 2000));
  for (const failure of ['app: server error', 'allocation failed', 'microphone delivery overflow', 'app: speech lost', 'speech buffer overflow: 100 ms dropped', 'speech record reservation failed', 'alloc 1900 failed', 'link: link: none', 'link: websocket error type=0', 'esp_transport_write() returned 0']) {
    assert.throws(() => inspectTurn(input + failure + '\n' + played));
  }
  assert.throws(() => inspectTurn(input + played.replace('gaps=0 (0 ms)', 'gaps=1 (20 ms)')));
  assert.throws(() => inspectTurn(input.replace('dropped=0ms', 'dropped=10ms') + played));
  assert.throws(() => inspectTurn(input + played.replace('via=wifi', 'via=usb')));
  assert.throws(() => inspectTurn(played));
  assert.throws(() => inspectTurn(input + played + '\napp: speech begin gen=5'));
});
function wav(seconds) {
  const bytes = seconds * 96000, buffer = Buffer.alloc(44 + bytes);
  buffer.write('RIFF'); buffer.writeUInt32LE(buffer.length - 8, 4); buffer.write('WAVEfmt ', 8);
  buffer.writeUInt32LE(16, 16); buffer.writeUInt16LE(1, 20); buffer.writeUInt16LE(1, 22);
  buffer.writeUInt32LE(48000, 24); buffer.writeUInt32LE(96000, 28);
  buffer.writeUInt16LE(2, 32); buffer.writeUInt16LE(16, 34);
  buffer.write('data', 36); buffer.writeUInt32LE(bytes, 40); return buffer;
}
test('native acoustic coverage checks samples against monotonic duration', () => {
  assert.equal(inspectCapture(wav(2), 'audio=2.001 wall=2.002').seconds, 2);
  assert.throws(() => inspectCapture(wav(2), 'audio=2 wall=2.5'));
  assert.throws(() => inspectCapture(wav(2).subarray(0, 100), 'audio=2 wall=2'));
  assert.throws(() => inspectCapture(wav(2), 'recording'));
});
