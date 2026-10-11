import assert from 'node:assert/strict';
import test from 'node:test';
import { VoiceTurnTrace } from '../src/voice-turn-trace.js';

test('native latency phases are relative, bounded and never log per audio frame', () => {
  let now = 100;
  const lines = [];
  const trace = new VoiceTurnTrace(s => lines.push(s), 'device', 7, () => now);
  now = 120; trace.phase('transcript');
  now = 150; trace.phase('provider-audio');
  for (let n=0;n<10000;n++) { now++; trace.phase('provider-audio'); }
  assert.deepEqual(lines, ['kubik: device turn 7 phase transcript +20ms',
    'kubik: device turn 7 phase provider-audio +50ms']);
  for (let n=0;n<100;n++) trace.phase(`phase-${n}`);
  assert.equal(lines.length, 24);
});
