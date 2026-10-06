import assert from 'node:assert/strict';
import test from 'node:test';
import { SpeechTrace } from '../src/speech-trace.js';
import { SpeechPacer } from '../src/audio.js';
import { acknowledgeNativeInput, sleep } from './helpers.js';
import { DeviceSession } from '../src/session.js';

test('speech diagnostics distinguish delayed source, device progress and socket pressure without payloads', async () => {
  let clock = 100, buffered = 0;
  const trace = new SpeechTrace(() => clock);
  const pacer = new SpeechPacer({ now: () => clock, progressWindowMs: 1200,
    bufferedAmount: () => buffered, sendAudio() {} });
  pacer.startGen(trace);
  const pcm = Buffer.alloc(1200 * 48);
  trace.source(pcm.length); pacer.push(pcm);
  clock += 800; trace.source(100 * 48); pacer.push(Buffer.alloc(100 * 48));
  clock += 300; pacer.played(100); await sleep(0);
  assert.equal(pacer.sentMs, 1300);
  buffered = 300_000;
  clock += 100; pacer.played(1300); trace.source(100 * 48); pacer.push(Buffer.alloc(100 * 48));
  clock += 200; buffered = 0; pacer.played(1300); await sleep(0);
  await pacer.drained();
  const stats = trace.summary();
  assert.equal(stats.sourceChunks, 3); assert.equal(stats.sourceMs, 1400);
  assert.equal(stats.sourceGapMs, 800); assert.equal(stats.sourceMaxMs, 1200);
  assert.equal(stats.sentFrames, 14); assert.equal(stats.sentMs, 1400);
  assert.equal(stats.maxInFlightMs, 1200); assert.equal(stats.creditWaitMs, 300);
  assert.equal(stats.socketWaitMs, 200); assert.equal(stats.socketMaxBytes, 300_000);
  assert.equal(stats.progressGapMs, 200); assert.equal(stats.sendGapMs, 1100);
  assert.ok(Object.values(stats).every(Number.isFinite));
});

test('session logs one numeric speech summary after playback or interruption', async (t) => {
  let port;
  const logs = [], events = [];
  const engine = { voiceMode: 'realtime', createVoiceTurn(options) { port = options.port; return { close() {} }; } };
  const session = new DeviceSession({ engine, device: { id: 'test' }, volume: 40, dispatch: async () => {},
    log: line => logs.push(line), ws: { readyState: 1, send(value) { acknowledgeNativeInput(session, value); if (typeof value === 'string') events.push(JSON.parse(value)); } } });
  t.after(() => session.close());
  for (const turn of [1, 2]) {
    session.handleMessage({ t: 'ptt', on: true, turn }); port.endInput();
    port.audio(Buffer.alloc(100 * 48)); port.audio(Buffer.alloc(200 * 48)); port.finish(); await sleep(0);
    const gen = events.findLast(event => event.t === 'speak_end').gen;
    assert.equal(logs.filter(line => /sourceChunks/.test(line)).length, turn - 1);
    if (turn === 1) session.handleMessage({ t: 'played', gen, ms: 300 });
    else session.interrupt('cancel');
    session.handleMessage({ t: 'played', gen, ms: 300 }); // a duplicate/late ACK must not log again
    const summaries = logs.filter(line => /sourceChunks/.test(line));
    assert.equal(summaries.length, turn);
    const stats = JSON.parse(summaries.at(-1).slice(summaries.at(-1).indexOf('{')));
    assert.equal(stats.sourceChunks, 2); assert.equal(stats.sentMs, 300);
    assert.ok(Object.values(stats).every(Number.isFinite));
  }
});

test('cancellation flushes an outstanding wait once and a broken diagnostic sink cannot block it', async () => {
  let clock = 0, result;
  const trace = new SpeechTrace(() => clock);
  const pacer = new SpeechPacer({ now: () => clock, progressWindowMs: 1200, sendAudio() {},
    onTrace: (_outcome, stats) => { result = stats; throw Error('Broken log sink'); } });
  pacer.startGen(trace); trace.source(2000 * 48); pacer.push(Buffer.alloc(2000 * 48));
  clock = 750;
  assert.doesNotThrow(() => pacer.finishTrace('cancel'));
  pacer.clear(); await pacer.drained();
  assert.equal(result.creditWaitMs, 750); assert.equal(trace.summary().creditWaitMs, 750);
  assert.equal(pacer.queuedBytes, 0);
});
