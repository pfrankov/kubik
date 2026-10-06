#!/usr/bin/env node
// On-device frame and voice acceptance through USB control; voice uses a local protocol mock by default.
//
//   nohup node tools/usb-bridge/bridge.mjs --port /dev/cu.usbmodem101 > /tmp/kubik-bridge.log 2>&1 &
//   node tools/test-device.mjs [--log /tmp/kubik-bridge.log] [--control http://127.0.0.1:18791]
//                              [--character Tess|Plush] [--skip-voice | --voice-only | --conversation-only] [--phrase "..."] [--speaker Milena]
//
// The bridge is the control channel (POST /config: the device's "sim" commands) and the log (device logs are
// framed into it). Runs several minutes and changes nothing that stays: volume and brightness
// are put back, the setup, pairing and card screens are closed. --character asserts the installed variant.
// --voice-only runs just the voice turns: start the bridge
// with `--server ws://127.0.0.1:18999/kubik/v1` for it (the mock below), a normal bridge for everything else.
//   1. frame statistics of every screen for the installed character (fps, mean and longest frame, late frames);
//   2. menu walk and hardware Back through simulated touches;
//   3. two voice turns: `say` speaks Russian to the device's microphone; the server must answer. By default the
//      server is a local mock (tools/usb-bridge/parrot-server.mjs, it echoes the speech): the test pauses the
//      device's Wi-Fi session (sim "wifi", nothing is saved, no setting or Wi-Fi credential is touched), the device
//      offers the USB route and the bridge, started with `--server ws://127.0.0.1:18999/kubik/v1`, reaches the mock.
//      Zero paid calls. The live server (real STT/LLM/TTS, it costs money) only with KUBIK_TEST_LIVE=1.
//   4. notifications: covered separately by tools/test-device-notify.mjs and test-device-radio.mjs.
// --conversation-only measures idle, recording, waiting, reply and interrupted transitions,
// then a physical voice turn; combine with --skip-voice for geometry/FPS alone.
// Full reset also changes the device key and requires new pairing; this non-destructive suite never performs it.
import fs from 'node:fs';
import { spawn, execFileSync } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { frameVerdict } from './test-device/frames.mjs';
import { startMock } from './test-device/mock.mjs';
import { flushStats, measureWindow, setDeviceScreen, waitForDeviceLink } from './test-device/diagnostics.mjs';

const args = process.argv.slice(2);
const option = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const CONTROL = option('--control', 'http://127.0.0.1:18791');
const LOG = option('--log', '/tmp/kubik-bridge.log');
const PHRASE = option('--phrase', 'Скажи одним коротким предложением, какого цвета небо.');
const SPEAKER = option('--speaker', 'Milena');
const SKIP_VOICE = args.includes('--skip-voice');
const VOICE_ONLY = args.includes('--voice-only');
const CONVERSATION_ONLY = args.includes('--conversation-only');
const ONLY_CHARACTER = option('--character', '');  // measure and speak with this character alone
if (ONLY_CHARACTER && !['Tess', 'Plush'].includes(ONLY_CHARACTER)) throw Error('Unknown character');

// Quiet animation uses the strict 30Hz/no-late gate. Finite transitions/reactions have separately measured
// baseline budgets; docs/acceptance.md explains frame work, cadence, sample count and late-count bounds.
// Key press -> first frame that shows the listening state: the frame being drawn when the key comes, then one more
// (the display task is woken at once; a frame takes at most 25 ms).
const KEY_TO_SCREEN_MS = 50;
const REPLY_LATENCY_MS = 8000;  // ptt_up -> speech header received; PCM timing is measured by the server
const MODE = { idle: 1, listening: 2, thinking: 3, speaking: 4, offline: 7 };  // face_mode_t
const CARD = 'Погода в Москве сегодня облачно, плюс двенадцать градусов, к вечеру возможен небольшой дождь.';
const MENU_TILE = { volume: [72, 226], brightness: [188, 226], reset: [240, 428] };
const LIVE = process.env.KUBIK_TEST_LIVE === '1';  // voice turns against the real server: they spend money
const MOCK_PORT = Number(process.env.KUBIK_TEST_MOCK_PORT || 18999);
const MOCK_HOLD_MS = 240000;  // the longest the Wi-Fi session stays paused (the test releases it earlier)
const screenMax = [];  // per-screen longest frame, printed as a table at the end

let failures = 0;
function check(name, ok, detail = '') {
  if (!ok) failures++;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}  ${detail}`.trimEnd());
  return ok;
}

// ---------------------------------------------------------------- device and log

async function device(command) {
  for (let attempt = 0; attempt < 5; attempt++) {
    const res = await fetch(`${CONTROL}/config`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(command),
      signal: AbortSignal.timeout(5000),
    });
    if (res.status !== 429) {
      const body = await res.json();
      if (!res.ok || !body.ok) throw Error(`Device rejected ${command.cmd}: HTTP ${res.status}`);
      return body;
    }  // 429: the bridge is still waiting for the previous reply
    await sleep(200);
  }
  throw new Error('bridge stays busy');
}
const sim = (ev, extra = {}) => device({ cmd: 'sim', ev, ...extra });

const logSize = () => fs.statSync(LOG).size;
function logSince(offset, size = logSize()) { // from the start if the bridge restarted
  const from = size < offset ? 0 : offset;
  if (size - from > 2 * 1024 * 1024) throw Error('Bridge log window exceeds 2 MiB');
  const buffer = Buffer.alloc(size - from);
  const fd = fs.openSync(LOG, 'r');
  fs.readSync(fd, buffer, 0, buffer.length, from);
  fs.closeSync(fd);
  return buffer.toString('utf8');
}
const clock = (line) => {  // bridge time stamp HH:MM:SS.mmm (UTC) in ms
  const m = /^(\d\d):(\d\d):(\d\d)\.(\d\d\d)/.exec(line);
  return m ? ((+m[1] * 60 + +m[2]) * 60 + +m[3]) * 1000 + +m[4] : NaN;
};
async function waitForLine(pattern, from, timeoutMs = 5000) {
  const until = Date.now() + timeoutMs;
  let carry = '';
  for (;;) {
    const end = logSize(), lines = (carry + logSince(from, end)).split('\n');
    from = end;
    carry = lines.pop().slice(-2048);
    const line = lines.find((l) => pattern.test(l));
    if (line) return line;
    if (Date.now() > until) return null;
    await sleep(40);
  }
}

// ---------------------------------------------------------------- frame statistics

// The display task logs its statistics when told to: the first call closes whatever window was running,
// the second returns a window that covers exactly the time in between.
const diagnostics = () => flushStats({ sim, logSize, logSince });
async function beginWindow() {
  await diagnostics();
  return logSize();
}
async function endWindow(start) {
  const stats = await diagnostics();
  stats.slow = logSince(start).split('\n').filter((line) => /main: slow frame/.test(line));
  return stats;
}
async function frameWindow(ms) {
  return measureWindow(async () => {
    const start = await beginWindow();
    await sleep(ms);
    return endWindow(start);
  });
}

function judgeFrames(name, s, kind = 'stable') {
  const ok = frameVerdict(s, kind);
  const detail = `${s.fps.toFixed(1)} fps, mean ${s.meanMs.toFixed(1)} ms, max ${s.maxMs.toFixed(1)} ms, late ${s.late}/${s.frames}, ${Math.round(s.px)} px, shown gap ${s.gapMs.toFixed(1)} ms [${kind}]`;
  check(`frames ${name}`, ok, detail);
  screenMax.push([name, s.maxMs, s.meanMs]);
  if (!ok) {  // where the time of an average frame goes (us): face, then the stages of render and send
    console.log(`      profile us/frame: face ${s.face}, prep ${s.prep}, image ${s.image}, shapes ${s.shapes}, glass ${s.glass}, ` +
                `hash ${s.hash}, send ${s.send}`);
    for (const line of s.slow) console.log(`      ${line}`);
  }
}

// ---------------------------------------------------------------- screens

const MOODS = ['calm', 'curious', 'playful', 'loved', 'scared', 'grumpy', 'sad', 'sleepy'];
const setMood = (mood) => sim('mood', { mood });
async function forceMode(mode) {
  const from = logSize();
  await sim('mode', { mode, ms: 20000 });
  const expected = new RegExp(`main: mode ${mode} on screen`);
  if (!await waitForLine(expected, from)) throw Error(`Mode ${mode} was not shown on the panel`);
}
const SCREENS = [
  { name: 'idle', enter: () => forceMode(MODE.idle) },
  { name: 'listening', enter: () => forceMode(MODE.listening) },
  { name: 'thinking', enter: () => forceMode(MODE.thinking) },
  { name: 'speaking', enter: () => forceMode(MODE.speaking) },
  { name: 'offline', enter: () => forceMode(MODE.offline) },
  { name: 'pairing code', enter: () => sim('pair', { code: 'ABCD2345' }), leave: () => sim('pair', { code: '' }) },
  { name: 'text card', enter: () => device({ cmd: 'card', text: CARD }), leave: () => device({ cmd: 'card', text: '' }) },
  { name: 'settings menu', enter: () => sim('menu'), leave: () => sim('menu') },
  // The setup card arrives 0.5..1 s after the command (the link is stopped and the access point started first) as a
  // few full-screen frames, a transition (bus-bound, 36..43 ms each), not steady state: measured after it.
  { name: 'wifi setup', enter: () => sim('setup'), leave: () => sim('menu'), settle: 1800, after: 2600 },  // "setup closed" bubble
  { name: 'rubbing to the reward', active: true, enter: () => sim('rub', { ms: 7000 }), settle: 200, window: 6500, after: 4500 },  // the LOVE reaction and its hearts fade before the next screen
  { name: 'taps, shake and pet', active: true, enter: () => void touches(), settle: 100, window: 3600, after: 3000 },  // the play layer: reactions, cues, swings
  // Tess's moods: each held for a window (forced by the hook, which also keeps it from fading), and the worst stacks: the colours,
  // the camera and the dots' own effects on top of a rub with a finger, a burst, a fling.
  ...MOODS.map((mood) => ({ name: `mood ${mood}`, only: 'Tess', enter: () => setMood(mood), leave: () => setMood('calm'), settle: 600, window: 3000, after: 300 })),
  { name: 'loved + rub + finger', active: true, only: 'Tess', enter: () => stacked('loved', [['rub', { ms: 7000 }], ['pet', { x: 240, y: 255 }, 2500]]), leave: () => setMood('calm'), settle: 200, window: 6500, after: 4500 },
  { name: 'scared + burst', active: true, only: 'Tess', enter: () => stacked('scared', [['shake', { x: 240, y: 255 }]]), leave: () => setMood('calm'), settle: 100, window: 3600, after: 3000 },
  { name: 'playful + fling', active: true, only: 'Tess', enter: () => stacked('playful', [['rub', { ms: 3000 }], ['tap', { x: 240, y: 255 }, 3200]]), leave: () => setMood('calm'), settle: 200, window: 4500, after: 3000 },
];

// The mood at once, then each of the touches [event, arguments, ms later (0: now)].
async function stacked(mood, touches) {
  await setMood(mood);
  for (const [ev, args, later = 0] of touches) {
    if (later) setTimeout(() => void sim(ev, args), later);
    else await sim(ev, args);
  }
}

async function touches() {
  for (const ev of ['tap', 'tap', 'tap', 'tap', 'tap', 'tap', 'shake', 'pet']) {
    await sim(ev, { x: 240, y: 255 });
    await sleep(300);
  }
}

// A hard shake of Tess: the dots fly over the whole panel, shiver, and are pulled home (2.65 s). The window opens before
// the shake, so every frame of the burst, the struggle and the return is counted.
async function measureBurst(character) {
  await sim('mode', { mode: MODE.idle, ms: 0 });
  await sleep(3000);
  const start = await beginWindow();
  await sim('shake', { x: 240, y: 255 });
  await sleep(3600);
  judgeFrames(`${character} hard shake burst`, await endWindow(start), 'reaction');
  await sleep(3000);
}

function judgeScreen(character, screen, stats) {
  if (stats.character !== character) check(`character during ${screen.name}`, false, `${stats.character}, expected ${character}`);
  judgeFrames(`${character} ${screen.name}`, stats, screen.active ? 'reaction' : 'stable');
}

async function measureScreens(character) {
  for (const screen of SCREENS) {
    if (CONVERSATION_ONLY && !['idle', 'listening', 'thinking', 'speaking'].includes(screen.name)) continue;
    if (screen.only && screen.only !== character) continue;
    await screen.enter();
    await sleep(screen.settle ?? 600);
    const stats = await frameWindow(screen.window ?? 2000);
    judgeScreen(character, screen, stats);
    await sim('mode', { mode: MODE.idle, ms: 0 });
    await screen.leave?.();
    await sleep(screen.after ?? 300);
    await waitForLink();
  }
  if (CONVERSATION_ONLY) await measureConversation(character);
  else if (character === 'Tess') await measureBurst(character);
}

// Include the approach, interrupted reply and retreat, not just settled geometry.
async function measureConversation(character) {
  const start = await beginWindow();
  for (const mode of [MODE.thinking, MODE.speaking, MODE.thinking, MODE.speaking, MODE.idle]) {
    await forceMode(mode);
    await sleep(mode === MODE.idle ? 4000 : 1800);
  }
  judgeFrames(`${character} conversation transitions`, await endWindow(start), 'conversation');
  await sim('mode', { mode: MODE.idle, ms: 0 });
}

// Setup closes the Wi-Fi session. Wait for its new route before measuring the next screen;
// reconnect frames belong to setup, not to the next animation window.
async function waitForLink() {
  return waitForDeviceLink(() => device({ cmd: 'info' }));
}

// ---------------------------------------------------------------- settings, menu, characters

async function calm() {  // no menu, setup or pairing screen open, and OpenClaw idle, when a step starts
  if (!(await waitForQuiet(120000))) console.log('WARN  OpenClaw stays busy with other work: bubbles on screen add render cost');
  const info = await device({ cmd: 'info' });
  if (info.setup_ap) await sim('menu');
  if (info.menu) await sim('menu');
  await sim('pair', { code: '' });
  await sim('mode', { mode: MODE.idle, ms: 0 });
  await sleep(700);
  return device({ cmd: 'info' });
}

async function openMenu() {
  const from = logSize();
  await sim('menu');
  const opened = await waitForLine(/settings menu open/, from);
  await sleep(550); // controls become active after the opening animation
  return opened;
}
async function closeMenu() {
  const from = logSize();
  await sim('boot');
  return waitForLine(/settings menu closed/, from);
}
async function tapTile(tile) {
  await sim('tap', { x: MENU_TILE[tile][0], y: MENU_TILE[tile][1] });
  await sleep(500);
}

async function menuWalk(original) {
  await sleep(500);
  check('menu opens', !!(await openMenu()));
  check('menu is open in info', (await device({ cmd: 'info' })).menu === true);
  const volume = original.volume;
  await tapTile('volume');
  const moved = await device({ cmd: 'info' });
  check('menu volume tile sets the volume', moved.volume !== volume, `${volume} -> ${moved.volume}`);
  await sim('drag', { x: 72, y: 226, first: true });
  await sim('hold', { x: 72, y: 226 });
  const sound = await device({ cmd: 'info' });
  check('volume hold opens Sound', sound.sound_menu === true);
  await sim('tap', { x: 348, y: 300 });
  const split = await device({ cmd: 'info' });
  check('interface slider is independent', split.ui_volume !== sound.ui_volume && split.volume === sound.volume);
  await sim('boot'); await sleep(250);
  check('BOOT returns from Sound to Settings', !(await device({ cmd: 'info' })).sound_menu && (await device({ cmd: 'info' })).menu);
  await tapTile('brightness');
  const dimmed = await device({ cmd: 'info' });
  check('menu brightness tile sets the brightness', dimmed.brightness !== original.brightness, `${original.brightness} -> ${dimmed.brightness}`);
  for (let n = 0; n < 5; n++) await sim('tap', { x: 432, y: 36 });
  await tapTile('reset');  // first tap only arms; closing the menu cancels confirmation
  check('unlocked reset needs confirmation', (await device({ cmd: 'info' })).ssid === original.ssid);
  check('menu closes', !!(await closeMenu()));
  check('menu is closed in info', (await device({ cmd: 'info' })).menu === false);
  await device({ cmd: 'set', volume, ui_volume: original.ui_volume, brightness: original.brightness });  // the menu saved the changed values
}

// ---------------------------------------------------------------- voice

function russianVoice() {
  try {
    return execFileSync('say', ['-v', '?'], { encoding: 'utf8' }).split('\n').some((l) => l.startsWith(SPEAKER) && /ru_RU/.test(l));
  } catch {
    return false;
  }
}
const speak = () => new Promise((resolve) => spawn('say', ['-v', SPEAKER, PHRASE], { stdio: 'ignore' }).on('close', resolve));

async function waitForCapture(previousReads) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const state = await device({ cmd: 'info' });
    if (state.mic_enabled && state.mic_rx_enabled && state.mic_reads > previousReads) return true;
    await sleep(40);
  }
  return false;
}

// One push-to-talk turn heard through the Mac speaker. Frame statistics are taken per phase.
async function voiceTurn(number) {
  const opening = await beginWindow();
  const from = logSize();
  const initial = await device({ cmd: 'info' });
  try { await recordAndReply(number, opening, from, initial.mic_reads); }
  finally { await sim('ptt_up'); } // a failed observation must not leave capture open for the next step
}

async function recordAndReply(number, opening, from, previousReads) {
  await sim('ptt_down');
  // Firmware diagnostics may intentionally drop when USB is busy; use actual ADC/RX/sample state.
  const listening = await waitForCapture(previousReads);
  const shown = await judgeKeyLatency(number, from);
  if (!shown) throw Error('No listening frame after KEY');
  if (!check(`turn ${number}: device listens`, listening)) throw Error('Physical microphone capture did not start');
  await settleFrom(shown, 350);
  judgeFrames(`real listening onset (turn ${number})`, await endWindow(opening), 'onset');
  // endWindow has already reset the counters; reuse that boundary for the next phase.
  const heard = logSize();
  await speak();
  await sleep(300);
  const listenStats = await endWindow(heard);
  const releasing = logSize();
  const releaseFrom = logSize();
  await sim('ptt_up');
  const thinking = await waitForLine(/main: mode 3 on screen/, releaseFrom);
  if (!check(`turn ${number}: thinking shown`, !!thinking)) throw Error('No thinking frame after PTT release');
  await settleFrom(thinking, 800);
  judgeFrames(`real reply transition (turn ${number})`, await endWindow(releasing), 'release');
  const waiting = logSize();
  const upLine = await waitForLine(/"t":"ptt","on":false/, from);
  const ms = Number(/"ms":(\d+)/.exec(upLine ?? '')?.[1]);
  check(`turn ${number}: microphone audio sent`, !!upLine && ms > 1500, `${ms} ms of speech`);
  const begin = await waitForLine(/app: speech begin/, from, 30000);
  if (!check(`turn ${number}: server replies with speech`, !!begin)) return;
  const thinkStats = await endWindow(waiting);
  const speaking = logSize();
  const latency = clock(begin) - clock(upLine);
  check(`turn ${number}: ptt_up to speech header`, latency > 0 && latency <= REPLY_LATENCY_MS, `${(latency / 1000).toFixed(2)} s`);
  const gen = Number(/gen=(\d+)/.exec(begin)?.[1]);
  const played = await waitForLine(new RegExp(`dev→srv .*"t":"played","gen":${gen},`), from, 60000);
  const speakStats = await endWindow(speaking);
  verifyPlayback(number, from, played);
  judgeFrames(`real listening (turn ${number})`, listenStats);
  judgeFrames(`real waiting for the reply (turn ${number})`, thinkStats);
  judgeFrames(`real speaking (turn ${number})`, speakStats);
}

function verifyPlayback(number, from, played) {
  const cancelled = /app: tap at /.test(logSince(from));
  const playedMs = Number(/"ms":(\d+)/.exec(played ?? '')?.[1]); // actual firmware ACK; zero means every speaker frame was dropped
  const detail = cancelled ? 'interrupted by a touch' : played?.slice(played.indexOf('{'));
  check(`turn ${number}: speech played to the end`, !!played && !cancelled && playedMs > 1000, detail);
}

// Key to screen, on the device's own clock: the app logs "key down", the display task logs the first frame of
// the new mode.
const tick = (line) => Number(/\((\d+)\) /.exec(line ?? '')?.[1]);
async function judgeKeyLatency(number, from) {
  const shown = await waitForLine(/main: mode 2 on screen/, from);
  const ms = tick(shown) - tick(logSince(from).split('\n').find((l) => /app: key down/.test(l)));
  check(`turn ${number}: key press to listening on screen`, ms >= 0 && ms <= KEY_TO_SCREEN_MS, `${ms} ms (limit ${KEY_TO_SCREEN_MS})`);
  return shown;
}

// Bridge timestamps and Date.now are UTC; account for a transition crossing midnight.
async function settleFrom(line, durationMs) {
  const elapsed = (Date.now() % 86400000 - clock(line) + 86400000) % 86400000;
  await sleep(Math.max(0, durationMs - elapsed));
}

// OpenClaw may be busy with other work (a "Coding" bubble is on screen then): frame statistics and voice turns need
// a quiet server, so wait for the activity line to go empty. Returns false when it never does.
async function waitForQuiet(timeoutMs = 25000) {
  const until = Date.now() + timeoutMs;
  for (;;) {
    const line = logSince(Math.max(0, logSize() - 20000)).split('\n').filter((l) => /app: activity/.test(l)).pop();
    if (!line || /own= other=$/.test(line.trim())) return true;
    if (Date.now() > until) return false;
    await sleep(500);
  }
}

// ---------------------------------------------------------------- the local mock server (default for voice)

// Starts the mock and pauses the device's Wi-Fi session so that it asks for the USB route, whose upstream is the mock
// (the bridge must have been started with --server pointing at it). Nothing is saved on the device. Returns
// { stop } once a device has reached the mock, or throws with the missing prerequisite - never the live server.
// Two successive turns in the installed character: against the local mock, or (KUBIK_TEST_LIVE=1 only) the live server.
async function voiceSession() {
  let mock = null;
  if (LIVE) console.warn('WARNING  KUBIK_TEST_LIVE=1: these voice turns go to the live server and COST MONEY (speech recognition, model, speech synthesis).');
  try {
    if (!LIVE) mock = await startMock({ device, sim, port: MOCK_PORT, holdMs: MOCK_HOLD_MS });
    for (const n of [1, 2]) {
      if (LIVE) await waitForQuiet(120000);
      await voiceTurn(n);
      if (LIVE) await waitForQuiet();
      await sleep(1000 + n * 17);
    }
  } finally {
    await mock?.stop();
  }
}

function voiceTurns() {
  // Missing logs only: close the mock and repeat one whole fresh session. Never repeat paid calls.
  return LIVE ? voiceSession() : measureWindow(voiceSession, () =>
    console.warn('WARN  Incomplete USB frame diagnostics; restarting one complete local-mock voice session'));
}

// ---------------------------------------------------------------- run

let original;  // the settings found on the device

// The installed variant, then the menu walk.
async function measureDevice() {
  await measureScreens(original.character);
  if (!CONVERSATION_ONLY) await menuWalk(await calm());
}

function requireCharacter(actual) {
  if (!['Tess', 'Plush'].includes(actual)) throw Error(`Unknown firmware character: ${actual}`);
  if (ONLY_CHARACTER && actual !== ONLY_CHARACTER) throw Error(`Flash ${ONLY_CHARACTER} before this check; found ${actual}`);
}

async function main() {
  const started = Date.now();
  try {
    original = VOICE_ONLY ? await device({ cmd: 'info' }) : await waitForLink();
  } catch (e) {
    console.error(`Cannot reach the bridge at ${CONTROL} (${e.message}). Start it:\n  nohup node tools/usb-bridge/bridge.mjs --port /dev/cu.usbmodem101 > ${LOG} 2>&1 &`);
    process.exit(2);
  }
  requireCharacter(original.character);
  check('device online through the bridge', original.ok && (original.online || VOICE_ONLY), `${original.device} fw ${original.fw} via ${original.via}`);  // voice-only: the mock is not up yet
  check('bridge log readable', fs.existsSync(LOG), LOG);
  try {
    await setDeviceScreen(() => device({ cmd: 'info' }), sim, false);
    await calm();
    if (!VOICE_ONLY) await measureDevice();
    if (SKIP_VOICE) console.log('SKIP  voice turns (--skip-voice)');
    else if (!russianVoice()) throw new Error(`Required voice turn needs ${SPEAKER} installed for say; --skip-voice is only a partial screen check`);
    else await voiceTurns();
    console.log('SKIP  destructive full reset (host tests verify erase; new key needs new pairing)');
    console.log('INFO  notifications covered separately by test-device-notify.mjs and test-device-radio.mjs');
  } finally {
    await calm();
    await device({ cmd: 'set', volume: original.volume, brightness: original.brightness });
    if ((await device({ cmd: 'info' })).menu !== original.menu) { await sim('menu'); await sleep(550); }
    await setDeviceScreen(() => device({ cmd: 'info' }), sim, original.screen_dark);
    const now = await device({ cmd: 'info' });
    check('settings restored', now.character === original.character && now.volume === original.volume && now.brightness === original.brightness &&
          ['ssid', 'url', 'key', 'guide_done', 'via', 'wifi_connected', 'menu', 'screen_dark'].every((key) => now[key] === original[key]),
          `${now.character}, volume ${now.volume}, brightness ${now.brightness}`);
  }
  console.log('\nlongest frame per window (ms; stable 33.3, finite transition/reaction 41)');
  for (const [name, max, mean] of screenMax) console.log(`  ${name.padEnd(50)} max ${max.toFixed(1).padStart(5)}  mean ${mean.toFixed(1).padStart(5)}`);
  console.log(`\n${failures ? `${failures} check(s) FAILED` : 'all checks passed'} in ${((Date.now() - started) / 1000).toFixed(0)} s`);
  process.exit(failures ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
