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

test('a short final packet uses only its actual consumption credit', async (t) => {
  let sent = 0;
  const pacer = new SpeechPacer({ sendAudio: pcm => { sent += pcm.length / 48; } });
  t.after(() => pacer.clear());
  pacer.startGen(); pacer.push(Buffer.alloc(SPEECH_LEAD_MS * 48));
  for (let batch=0;batch<7;batch++) await settle();
  pacer.push(Buffer.alloc(20 * 48)); pacer.mark(() => {});
  assert.equal(sent, SPEECH_LEAD_MS);
  pacer.played(20); await pacer.drained();
  assert.equal(sent, SPEECH_LEAD_MS + 20);
});


test('large provider deltas use bounded I/O batches without granting wall-clock credit', async (t) => {
  let sent=0;
  const pacer = new SpeechPacer({sendAudio: pcm => sent+=pcm.length/48});
  t.after(()=>pacer.clear());
  pacer.startGen();pacer.push(Buffer.alloc(2000*48));
  assert.equal(sent,100);
  for (const expected of [200,300,400,500,600,700,800]) {await settle();assert.equal(sent,expected);}
  await sleep(30);assert.equal(sent,SPEECH_LEAD_MS);
  pacer.played(400);await sleep(30);assert.equal(sent,1200);
  assert.equal(sent-400,SPEECH_LEAD_MS);
  pacer.clear();await pacer.drained();
});

test('cancellation during an I/O yield discards queued frames and preserves a new generation', async (t) => {
  for (const delayed of [true, false]) {
    const frames=[];
    const pacer = new SpeechPacer({sendAudio:pcm=>frames.push(pcm[0])});
    t.after(()=>pacer.clear());
    pacer.startGen();pacer.push(Buffer.alloc(600*48,1));
    assert.deepEqual(frames,[1]);pacer.clear();
    if (delayed) await settle();
    assert.deepEqual(frames,[1]);
    pacer.startGen();pacer.push(Buffer.alloc(220*48,2));pacer.mark(()=>frames.push('end'));
    await pacer.drained();assert.deepEqual(frames,[1,2,2,2,'end']);
  }
});
