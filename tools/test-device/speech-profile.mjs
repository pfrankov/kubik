import assert from 'node:assert/strict';
import { openSync, readSync, closeSync, statSync, readFileSync } from 'node:fs';
import { setTimeout as sleep } from 'node:timers/promises';
import { flushStats, measureWindow } from './diagnostics.mjs';
import { frameVerdict } from './frames.mjs';

export const PROFILE_MS = 8000;
async function waitForLog(check, label, timeoutMs) {
  const deadline = performance.now() + timeoutMs;
  while (!check() && performance.now() < deadline) await sleep(40);
  assert.ok(check(), label);
}
export function speechProfile({ sim, info, burst, chunkMs = 100, measureFrames = true }) {
  const source = process.env.KUBIK_TEST_SPEECH_PCM ? readFileSync(process.env.KUBIK_TEST_SPEECH_PCM) : null;
  if (source) assert.equal(source.length, PROFILE_MS * 48, 'Fixture must contain 8 seconds of mono 24 kHz PCM16');
  const path = process.env.KUBIK_DEVICE_LOG ?? '/tmp/kubik-bridge.log';
  const logSize = () => statSync(path).size;
  const logSince = (offset) => {
    const size = logSize() - offset;
    assert.ok(size >= 0 && size <= 2 * 1024 * 1024, 'Invalid diagnostic log window');
    const fd = openSync(path, 'r'), bytes = Buffer.alloc(size);
    try { readSync(fd, bytes, 0, size, offset); return bytes.toString(); } finally { closeSync(fd); }
  };
  const stats = () => flushStats({ sim, logSize, logSince });
  async function sample(mode) {
    const frames = await measureWindow(stats);
    console.log(`PROFILE ${mode}: ${JSON.stringify(frames)}`);
    assert.ok(frameVerdict(frames, 'conversation'), `${mode} speech animation exceeded its frame budget`);
  }
  return async (callbacks, closed, mode) => {
    const from = logSize();
    if (measureFrames) await stats();
    const started = performance.now();
    const presentedSpeech = () => /main: mode 4 on screen/.test(logSince(from));
    // USB measurements must not pause the simulated network producer.
    const measurement = measureFrames ? (async () => {
      await waitForLog(presentedSpeech, 'Speech never reached the screen', 2000);
      await stats();
      const measuring = performance.now();
      for (const at of [2200, 4600, 7000]) {
        await sleep(Math.max(0, measuring + at - performance.now()));
        await sample(mode);
      }
    })() : Promise.resolve();
    measurement.catch(() => {}); // Await below; avoid an unhandled rejection while producing PCM.
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
    callbacks.onResponseDone({ status: 'completed' });
    await measurement;
    // played includes transport credit and the hardware tail; a fixed sleep can cancel valid playback.
    const completion = /speech played gen=\d+ ms=(\d+) elapsed=(\d+) gaps=(\d+) \((\d+) ms\)/;
    await waitForLog(() => completion.test(logSince(from)), 'Missing physical playback completion diagnostics', 5000);
    const log = logSince(from);
    assert.doesNotMatch(log, /speech buffer|reservation failed|alloc \d+ failed|read error|PANIC|assert failed/);
    const played = completion.exec(log);
    assert.ok(played, 'Missing physical playback completion diagnostics');
    assert.equal(Number(played[1]), PROFILE_MS);
    assert.equal(Number(played[3]), 0, 'Audio starved during the bounded local fixture');
    console.log(`PASS ${mode} ${burst ? 'burst' : 'paced'} ${chunkMs}ms chunks: ${played[0]}`);
  };
}
