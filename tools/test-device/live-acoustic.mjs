import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { readFileSync, statSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';
import { EchoCleaner } from '../../openclaw-kubik/src/echo-cleaner.js';

const run = (command, args) => new Promise((resolve, reject) => {
  const child = spawn(command, args, { stdio: ['ignore', 'inherit', 'inherit'], timeout: 30000 });
  child.once('error', reject);
  child.once('close', code => code === 0 ? resolve() : reject(Error(`${command} failed (${code})`)));
});

// An external computer speaker supplies known near-end speech, independently
// of the Cube's DAC reference. No provider or speech-recognition API is called.
export async function nearSpeechProof(state, { info, hostUntil }) {
  const wav = process.env.KUBIK_TEST_NEAR_WAV;
  assert.equal(process.platform, 'darwin', 'Near-end fixture uses macOS afplay');
  assert.ok(wav && state.samples, 'Set KUBIK_TEST_NEAR_WAV and use --echo --near');
  assert.ok(statSync(wav).size <= 1024 * 1024, 'Near-end WAV exceeds the fixture bound');
  assert.ok(process.env.KUBIK_TEST_SPEECH_PCM, 'Set KUBIK_TEST_SPEECH_PCM to a different spoken phrase');
  assert.equal(statSync(process.env.KUBIK_TEST_SPEECH_PCM).size, 8 * 48000, 'Far-end phrase must be eight seconds');
  const farSpeech = readFileSync(process.env.KUBIK_TEST_SPEECH_PCM).subarray(0, 5 * 48000);
  const cleaner = new EchoCleaner(), dir = mkdtempSync(join(tmpdir(), 'kubik-near-'));
  let passed = false;
  const clean = () => {
    const frames = state.samples.splice(0); // Keep each phase within the 30 s capture bound.
    return { mic: Buffer.concat(frames.map(frame => frame.mic)),
      clean: Buffer.concat(frames.map(frame => cleaner.process(frame.mic, frame.reference))),
      reference: Buffer.concat(frames.map(frame => frame.reference)) };
  };
  try {
    const source = join(dir, 'source.wav');
    writeFileSync(source, readFileSync(wav), { mode: 0o400 }); // Same immutable source in both phases.
    clean(); // Train on the preceding physical far-end replies.
    for (const label of ['near', 'double']) {
      const receipts = state.receipts.length, input = state.audioFrames;
      if (label === 'double') {
        const previousGen = state.progress?.gen;
        state.liveCallbacks.onAudio(farSpeech);
        await hostUntil(() => state.progress?.gen !== previousGen && state.progress?.ms > 0, 'double-talk starts playing');
        clean(); // Begin the acoustic window after the hardware actually starts.
      }
      await run('afplay', [source]); await sleep(500);
      assert.ok(state.audioFrames > input + 100, 'Near speech did not stream continuously');
      assert.equal((await info()).mic_open, true);
      const frames = clean();
      assert.ok(frames.mic.length >= 5 * 48000, 'Physical near-end fixture is incomplete');
      writeFileSync(join(dir, `${label}-mic.pcm`), frames.mic);
      writeFileSync(join(dir, `${label}-clean.pcm`), frames.clean);
      writeFileSync(join(dir, `${label}-reference.pcm`), frames.reference);
      if (label === 'double') await hostUntil(() => state.receipts.length > receipts, 'double-talk playback ACK');
    }
    await run('uv', ['run', '--with', 'numpy==2.2.6', 'python',
      'tools/test-device/analyze-live-acoustic.py', source, dir]);
    console.log('PASS physical near-end and double-talk speech retained');
    passed = true;
  } finally {
    cleaner.close();
    if (passed) rmSync(dir, { recursive: true, force: true });
    else console.error(`Failed acoustic captures retained in private directory: ${dir}`);
  }
}
