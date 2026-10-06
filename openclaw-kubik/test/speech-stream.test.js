import assert from 'node:assert/strict';
import test from 'node:test';
import { SpeechStream } from '../src/speech-stream.js';

const tick = () => new Promise((resolve) => setImmediate(resolve));

test('slow playback bounds queued segments and preserves every segment in order', async () => {
  const stream = new SpeechStream('reply', 1);
  let accepted = 0, peak = 0;
  const producer = (async () => {
    for (let i = 0; i < 100; i++) {
      assert.equal(await stream.enqueue({ text: String(i) }), true);
      accepted++; peak = Math.max(peak, stream.items.length);
    }
  })();
  await tick();
  assert.equal(accepted, 32);
  const spoken = [];
  for (let i = 0; i < 100; i++) {
    spoken.push(stream.take().text);
    await tick();
  }
  await producer;
  assert.equal(peak, 32);
  assert.deepEqual(spoken, Array.from({ length: 100 }, (_, i) => String(i)));
});

test('interruption releases a blocked producer and discards only the cancelled speech', async () => {
  const stream = new SpeechStream('reply', 1);
  for (let i = 0; i < 32; i++) await stream.enqueue({ text: 'queued' });
  let resumed = false;
  const pending = stream.enqueue({ text: 'blocked' }).then((ok) => { resumed = true; return ok; });
  await tick(); assert.equal(resumed, false);
  stream.cancel();
  assert.equal(await pending, false);
  assert.equal(stream.items.length, 0);
  assert.equal(await stream.enqueue({ text: 'late' }), false);
});
