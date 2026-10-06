// Opt-in, one paid production turn. Never invoked by accept.py or retried automatically.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { parseArgs } from 'node:util';
import { setTimeout as sleep } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { assertIdleCapture } from './test-device/diagnostics.mjs';
import { inspectTurn, inspectCapture } from './test-device/acoustic.mjs';

const { values: options } = parseArgs({ options: {
  'allow-paid': { type: 'boolean' }, phrase: { type: 'string' }, voice: { type: 'string', default: 'Milena' },
  log: { type: 'string', default: '/tmp/kubik-bridge.log' },
  control: { type: 'string', default: 'http://127.0.0.1:18791' },
  volume: { type: 'string', default: '55' },
} });
assert.ok(options['allow-paid'], 'Requires explicit --allow-paid; makes one production provider/agent call');
assert.equal(process.platform, 'darwin', 'Native acoustic recorder requires macOS');
assert.ok(options.phrase?.trim() && options.phrase.length <= 1000, 'Provide --phrase (1–1000 characters)');
const volume = Number(options.volume);
assert.ok(Number.isInteger(volume) && volume >= 20 && volume <= 80, 'Volume must be 20–80');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'kubik-acoustic-'));
fs.chmodSync(directory, 0o700);
const base = path.join(directory, 'turn'), offset = fs.statSync(options.log).size;
const result = { started: new Date().toISOString(), events: [], result: 'failed' };
console.log(`One paid KEY-triggered turn; private recording: ${directory}`);

async function control(body) {
  const response = await fetch(`${options.control}/config`, { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body), signal: AbortSignal.timeout(5000) });
  assert.equal(response.status, 200, `Control ${body.cmd}`);
  const data = await response.json(); assert.equal(data.ok, true); return data;
}
const info = () => control({ cmd: 'info' });
const sim = ev => control({ cmd: 'sim', ev });
function logWindow() {
  const size = fs.statSync(options.log).size - offset;
  assert.ok(size >= 0 && size <= 2_000_000, 'Device log rotated or exceeded bounded window');
  const fd = fs.openSync(options.log, 'r'), bytes = Buffer.alloc(size);
  try { fs.readSync(fd, bytes, 0, size, offset); } finally { fs.closeSync(fd); }
  return bytes.toString();
}
function event(type) {
  const item = { type, at: new Date().toISOString() };
  result.events.push(item); console.log(JSON.stringify(item));
}
function child(command, args) {
  const process = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  const state = { process, text: '' };
  process.stdout.on('data', b => { state.text += b; });
  process.stderr.on('data', b => { state.text += b; });
  state.done = new Promise(resolve => {
    process.once('error', error => resolve({ code: -1, error: error.message }));
    process.once('close', (code, signal) => resolve({ code, signal }));
  });
  return state;
}
async function until(check, ms, label) {
  const deadline = Date.now() + ms;
  do { if (interrupted) throw Error(interrupted); if (await check()) return; await sleep(200); } while (Date.now() < deadline);
  throw Error(`Timed out: ${label}`);
}
function setVolume(value) { execFileSync('osascript', ['-e', `set volume output volume ${value}`], { timeout: 5000 }); }
const original = await info();
assert.ok(original.online && original.via === 'wifi' && original.voice_mode === 'realtime', 'Requires production Wi-Fi Realtime');
assert.ok(!original.mic_open && !original.menu && original.character === 'Tess', 'Requires idle Tess home screen');
assert.ok(original.volume >= 20, 'Device voice volume is below 20');
assert.ok(Number.isInteger(original.heap_failures), 'Firmware must expose allocation-failure counter');
const originalVolume = Number(execFileSync('osascript', ['-e', 'output volume of (get volume settings)'], { encoding: 'utf8', timeout: 5000 }).trim());
const originalMute = execFileSync('osascript', ['-e', 'output muted of (get volume settings)'], { encoding: 'utf8', timeout: 5000 }).trim();
let recorder, player, triggered = false, questionMs = 0, interrupted;
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => {
  interrupted = `Interrupted by ${signal}`;
  player?.process.kill('SIGTERM');
});
async function terminate(state) {
  if (!state || state.process.exitCode !== null || state.process.signalCode) return;
  state.process.kill('SIGTERM');
  let ended = await Promise.race([state.done, sleep(1000).then(() => null)]);
  if (!ended) { state.process.kill('SIGKILL'); ended = await Promise.race([state.done, sleep(1000).then(() => null)]); }
  assert.ok(ended, 'Child process did not exit');
}

async function recordQuestion() {
  const swift = fileURLToPath(new URL('./test-device/acoustic-recorder.swift', import.meta.url));
  execFileSync('xcrun', ['swiftc', swift, '-o', `${base}.recorder`], { timeout: 60000 });
  try { execFileSync('say', ['-v', options.voice, '-o', `${base}.question.aiff`, options.phrase], { timeout: 30000 }); }
  catch { throw Error('Question synthesis failed'); }
  recorder = child(`${base}.recorder`, [`${base}.wav`, `${base}.stop`]);
  await until(() => recorder.text.includes('recording\n'), 5000, 'recorder readiness');
  event('recorder_ready'); await sleep(1500);
  if (interrupted) throw Error(interrupted);
  setVolume(volume); execFileSync('osascript', ['-e', 'set volume without output muted'], { timeout: 5000 });
  await sim('stats'); triggered = true; await sim('ptt_down'); event('trigger');
  await until(async () => { const s = await info(); return s.mic_open && s.mic_rx_enabled && s.mic_reads > original.mic_reads; }, 5000, 'microphone start');
  await sleep(200); event('question_start');
  const questionStart = performance.now();
  player = child('afplay', [`${base}.question.aiff`]);
  const played = await Promise.race([player.done, sleep(30000, undefined, { ref: false }).then(() => ({ code: -1 }))]); assert.equal(played.code, 0, 'Question playback failed');
  questionMs = performance.now() - questionStart;
  event('question_end');
}
async function awaitReply() {
  await until(() => { result.turn = inspectTurn(logWindow(), questionMs - 200); return result.turn !== null; }, 90000, 'complete response');
  const state = await info();
  assert.ok(!state.mic_open, 'Microphone did not close after input');
  assert.ok(state.online && state.via === 'wifi', 'Production Wi-Fi lost');
  event('speech_played'); await sleep(1600); await sim('stats');
  inspectTurn(logWindow(), questionMs - 200); // Include recovery errors after played.
  const recovered = await info();
  assertIdleCapture(recovered);
  assert.equal(recovered.heap_failures, original.heap_failures, 'Allocation failed during the real turn');
}
async function stopRecorder() {
  if (!recorder) return;
  fs.writeFileSync(`${base}.stop`, 'stop', { mode: 0o600 });
  const status = await Promise.race([recorder.done, sleep(5000, undefined, { ref: false }).then(() => null)]);
  if (!status) { await terminate(recorder); throw Error('Recorder did not stop'); }
  fs.writeFileSync(`${base}.capture.log`, recorder.text, { mode: 0o600 });
  assert.equal(status.code, 0, 'Recorder failed');
  result.capture = inspectCapture(fs.readFileSync(`${base}.wav`), recorder.text);
}
async function restoreDevice() {
  const state = await info();
  if (triggered && state.mic_open) { await sim('ptt_up'); await sleep(500); }
  if (state.screen_dark !== original.screen_dark) { await sim('pwr'); await sleep(1800); }
  const restored = await info();
  assertIdleCapture(restored);
  assert.equal(restored.screen_dark, original.screen_dark, 'Screen restoration failed');
}
function restoreVolume() {
  setVolume(originalVolume);
  execFileSync('osascript', ['-e', `set volume output muted ${originalMute}`], { timeout: 5000 });
  const volumeNow = Number(execFileSync('osascript', ['-e', 'output volume of (get volume settings)'], { encoding: 'utf8', timeout: 5000 }).trim());
  assert.equal(volumeNow, originalVolume, 'Host volume restoration failed');
}
try {
  if (original.screen_dark) { await sim('pwr'); await sleep(1800); }
  await recordQuestion(); await awaitReply();
  if (interrupted) throw Error(interrupted);
  result.result = 'passed';
} catch (error) { result.error = error.message; }
finally {
  for (const cleanup of [() => terminate(player), stopRecorder, restoreDevice, restoreVolume]) {
    try { await cleanup(); } catch (error) { result.result = 'failed'; result.error = `${result.error ?? ''} ${error.message}`.trim(); }
  }
  try { fs.writeFileSync(`${base}.device.log`, logWindow(), { mode: 0o600 }); }
  catch (error) { result.result = 'failed'; result.error = error.message; }
  fs.writeFileSync(`${base}.json`, JSON.stringify(result, null, 2), { mode: 0o600 });
  console.log(JSON.stringify(result));
  process.exitCode = result.result === 'passed' ? 0 : 1;
}
