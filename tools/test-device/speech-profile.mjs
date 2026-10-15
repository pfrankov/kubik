import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setTimeout as sleep } from 'node:timers/promises';
import { flushStats } from './diagnostics.mjs';
import { frameVerdict } from './frames.mjs';

export const PROFILE_MS = 8000;
export function assertFaultWindow(current, previous) {
  assert.equal(current.ok, true);
  assert.ok(typeof current.instance === 'string' && current.instance.length > 0, 'Missing USB collector identity');
  assert.equal(current.instance, previous.instance, 'USB diagnostic collector restarted');
  for (const field of ['faults','boots','disconnects']) {
    assert.ok(Number.isInteger(current[field]) && current[field] >= 0 &&
      Number.isInteger(previous[field]) && previous[field] >= 0);
    assert.equal(current[field],previous[field], `Device ${field} changed during speech`);
  }
}
export function assertPlaybackWindow(frames, generation) {
  assert.equal(frames.presented, frames.frames, 'Frame window contains unpresented frames');
  assert.equal(frames.overflows, 0, 'Frame storage overflowed during speech');
  assert.equal(frames.modes, 1 << 4, 'Frame window crossed a non-speaking mode');
  assert.equal(frames.active, generation, 'Speech ended before frame diagnostics completed; this window is unmeasured');
  assert.ok(frameVerdict(frames, 'conversation'), 'Speech animation exceeded its frame budget');
}
export function assertPlaybackCompletion(played, previous) {
  for (const snapshot of [played, previous]) {
    for (const field of ['sequence', 'heap_failures'])
      assert.ok(Number.isInteger(snapshot[field]) && snapshot[field] >= 0, `Missing playback ${field}`);
  }
  assert.ok(played.sequence > previous.sequence && played.active < 0, 'Missing physical playback completion diagnostics');
  assert.ok(Number.isInteger(played.gen) && played.gen >= 0, 'Missing playback generation');
  assert.equal(played.reason, 'played', 'Physical playback was cancelled or lost');
  assert.equal(played.ms, PROFILE_MS);
  assert.equal(played.gaps, 0, 'Audio starved during the bounded fixture');
  assert.equal(played.gap_ms, 0, 'Audio starvation duration must be zero');
  assert.equal(played.rx_rejected, 0, 'Device rejected current-generation PCM');
  assert.equal(played.write_errors, 0, 'Speaker DMA write failed');
  assert.equal(played.late_feeds, 0, 'Speaker feed exceeded the DMA reserve');
  assert.equal(played.heap_failures, previous.heap_failures, 'Allocation failed during speech');
}
async function producePcm(callbacks, closed, { source, burst, chunkMs, started }) {
  for (let offset = 0; offset < PROFILE_MS; offset += chunkMs) {
    assert.equal(closed(), false, 'Native provider closed before all PCM was accepted');
    const samples = Math.min(chunkMs, PROFILE_MS - offset) * 24;
    const pcm = Buffer.alloc(samples * 2);
    for (let i = 0; i < samples; ++i) {
      const t = offset / 1000 + i / 24000;
      const envelope = .08 + .82 * ((1 + Math.sin(t * 2 * Math.PI * 3.7)) / 2) ** 2;
      pcm.writeInt16LE(Math.round(envelope * 15000 * (Math.sin(t * 2 * Math.PI * 180) + .25 * Math.sin(t * 2 * Math.PI * 360))), i * 2);
    }
    if (source) source.copy(pcm, 0, offset * 48, (offset + samples / 24) * 48);
    callbacks.onAudio(pcm);
    if (!burst) {
      await sleep(Math.max(0, started + offset + chunkMs - performance.now()));
    }
  }
}
export function speechProfile({ sim, burst, diagnostics, faults, chunkMs = 100, measureFrames = true }) {
  assert.equal(typeof faults, 'function', 'Physical profiles require the supplemental USB fault collector');
  const source = process.env.KUBIK_TEST_SPEECH_PCM ? readFileSync(process.env.KUBIK_TEST_SPEECH_PCM) : null;
  if (source) assert.equal(source.length, PROFILE_MS * 48, 'Fixture must contain 8 seconds of mono 24 kHz PCM16');
  const stats = () => flushStats({ sim });
  async function sample(mode, generation) {
    const frames = await stats();
    console.log(`PROFILE ${mode}: ${JSON.stringify(frames)}`);
    assertPlaybackWindow(frames, generation);
  }
  return async (callbacks, closed, mode) => {
    const previousFaults = await faults();
    assertFaultWindow(previousFaults,previousFaults);
    if (measureFrames) await stats();
    const previous = await diagnostics();
    const started = performance.now();
    // USB measurements must not pause the simulated network producer.
    const measurement = measureFrames ? (async () => {
      const deadline = performance.now() + 2000;
      let presented = false, generation;
      do {
        const frames = await stats();
        presented = Boolean(frames.modes & (1 << 4)) && frames.active >= 0;
        generation = frames.active;
        if (!presented) await sleep(40);
      } while (!presented && performance.now() < deadline);
      assert.ok(presented, 'Speech never reached the screen');
      // The matching snapshot already reset the display window. A second reset
      // wastes USB round-trip time and can push the final window past an 8 s reply.
      const measuring = performance.now();
      for (const at of [2200, 4600, 7000]) {
        await sleep(Math.max(0, measuring + at - performance.now()));
        await sample(mode, generation);
      }
    })() : Promise.resolve();
    measurement.catch(() => {}); // Await below; avoid an unhandled rejection while producing PCM.
    await producePcm(callbacks, closed, {source, burst, chunkMs, started});
    callbacks.onResponseDone({ status: 'completed' });
    let measurementError;
    try { await measurement; } catch (error) { measurementError = error; }
    // played includes transport credit and the hardware tail; a fixed sleep can cancel valid playback.
    let played;
    const deadline = started + PROFILE_MS + 5000;
    do {
      played = await diagnostics();
      if (played.sequence > previous.sequence && played.active < 0) break;
      await sleep(40);
    } while (performance.now() < deadline);
    console.log(`PLAYBACK ${mode}: ${JSON.stringify(played)}`);
    assertPlaybackCompletion(played, previous);
    assertFaultWindow(await faults(),previousFaults);
    console.log(`PASS ${mode} ${burst ? 'burst' : 'paced'} ${chunkMs}ms chunks: ${JSON.stringify(played)}`);
    if (measurementError) throw measurementError;
    return played;
  };
}
