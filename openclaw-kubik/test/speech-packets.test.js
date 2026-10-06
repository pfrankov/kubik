import assert from 'node:assert/strict';
import test from 'node:test';
import { SpeechPacer } from '../src/audio.js';
import { SPEECH_LEAD_MS } from '../src/protocol.js';
import { sleep } from './helpers.js';

const settle = () => new Promise(resolve => setImmediate(resolve));

test('20 ms provider deltas form one 100 ms packet, with an immediate final partial packet', async () => {
  let clock = 0;
  const frames = [], order = [];
  const pacer = new SpeechPacer({ now: () => clock, sendAudio: pcm => { frames.push(pcm); order.push('audio'); } });
  pacer.startGen();
  for (let i = 0; i < 5; i++) {
    pacer.push(Buffer.alloc(20 * 48, i)); await settle();
    assert.equal(frames.length, i === 4 ? 1 : 0);
    clock += 20;
  }
  assert.equal(frames[0].length, 100 * 48);
  for (let i = 0; i < 5; i++) assert.equal(frames[0][i * 20 * 48], i);
  pacer.push(Buffer.alloc(20 * 48, 9));
  pacer.mark(() => order.push('end'));
  await pacer.drained();
  assert.equal(frames[1].length, 20 * 48); assert.equal(frames[1][0], 9);
  assert.deepEqual(order, ['audio', 'audio', 'end']);
});

test('partial packet has a bounded wait and cancellation discards its bytes', async () => {
  const frames = [];
  const pacer = new SpeechPacer({ sendAudio: pcm => frames.push(pcm) });
  pacer.startGen(); pacer.push(Buffer.alloc(20 * 48, 1));
  assert.equal(frames.length, 0);
  await sleep(150); assert.equal(frames.length, 1, 'partial PCM waited indefinitely for the provider');
  pacer.push(Buffer.alloc(20 * 48, 2)); pacer.clear(); await pacer.drained();
  await sleep(110); assert.equal(frames.length, 1, 'cancelled bytes reached the device');
});

test('a short final packet uses only its actual consumption credit', async () => {
  let sent = 0;
  const pacer = new SpeechPacer({ sendAudio: pcm => { sent += pcm.length / 48; } });
  pacer.startGen(); pacer.push(Buffer.alloc(SPEECH_LEAD_MS * 48));
  pacer.push(Buffer.alloc(20 * 48)); pacer.mark(() => {});
  assert.equal(sent, SPEECH_LEAD_MS);
  pacer.played(20); await pacer.drained();
  assert.equal(sent, SPEECH_LEAD_MS + 20);
});
