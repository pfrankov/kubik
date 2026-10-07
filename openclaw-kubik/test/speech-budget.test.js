import assert from 'node:assert/strict';
import test from 'node:test';
import { SpeechPacer } from '../src/audio.js';
import { SPEECH_LEAD_MS } from '../src/protocol.js';

// Production credit and packet size, with both directions of the network delayed.
async function playback(rtt, reportMs, phase) {
  let clock = 0, held = 0, played = 0, peak = 0, gaps = 0, started = false, nextReport = phase;
  const packets = [], reports = [];
  const pacer = new SpeechPacer({ now: () => clock, sendAudio(pcm) {
    packets.push({ at: clock + rtt / 2, ms: pcm.length / 48 });
  } });
  try {
    pacer.startGen(); pacer.push(Buffer.alloc(5000 * 48));
    for (; clock < 10_000 && played < 5000; clock += 20) {
      while (reports[0]?.at <= clock) pacer.played(reports.shift().ms);
      await new Promise(resolve => setImmediate(resolve));
      while (packets[0]?.at <= clock) held += packets.shift().ms;
      peak = Math.max(peak, held);
      if (!started && held >= 600) started = true; // firmware/main/audio.c prebuffer
      if (started) {
        if (held >= 20) { held -= 20; played += 20; } else gaps++;
      }
      if (clock >= nextReport) {
        reports.push({ at: clock + rtt / 2, ms: played });
        nextReport += reportMs;
      }
    }
    return { played, peak, gaps };
  } finally { pacer.clear(); await pacer.drained(); }
}

test('production speech budget sustains 50–200 ms RTT, report phases and 420 ms progress cadence', async () => {
  assert.equal(SPEECH_LEAD_MS, 800);
  for (const rtt of [50, 120, 200]) {
    for (const reportMs of [100, 250, 300, 420]) {
      for (const phase of [0, 100, 240]) {
        const result = await playback(rtt, reportMs, phase);
        assert.equal(result.played, 5000);
        assert.equal(result.gaps, 0, JSON.stringify({ rtt, reportMs, phase, ...result }));
        assert.ok(result.peak <= 900, 'exceeded physical ring capacity');
      }
    }
  }
});

test('budget model detects a network delay beyond the available speech cushion', async () => {
  const result = await playback(600, 300, 240);
  assert.equal(result.played, 5000);
  assert.ok(result.gaps > 0, 'model concealed missing playback samples');
});
