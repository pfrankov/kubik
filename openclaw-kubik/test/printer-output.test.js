import assert from 'node:assert/strict';
import test from 'node:test';
import { PrinterChannel, PrinterDeviceSession } from '../src/printer-session.js';
import { SessionOutputLease } from '../src/session-output.js';
import { acknowledgeNativeInput, fakeEngine, sleep } from './helpers.js';
import { printerResult } from './printer-fixture.js';

async function until(predicate) {
  for (let i = 0; i < 200; i++) { if (predicate()) return; await sleep(5); }
  assert.fail('Session did not reach the expected state');
}

function fixture(t, { live = false, autoPlayed = true, autoCancelled = true } = {}) {
  const events = [], samples = new Map(), calls = { input: 0, reference: 0, starts: 0, closes: 0 };
  let port, session, allowed = true;
  const engine = fakeEngine({ speakDelayMs: 0 });
  if (live) Object.assign(engine, { voiceMode: 'live', nativeVoice: { available: true },
    createVoiceTurn(options) {
      calls.starts++; port = options.port;
      return { append(pcm, reference) { calls.input++; if (reference) calls.reference++; }, close() { calls.closes++; } };
    } });
  session = new PrinterDeviceSession({ device: { id: 'desk' }, engine, volume: 100, sessionId: 'lease-test',
    dispatch: async () => {}, authorize: async () => allowed,
    ws: { readyState: 1, bufferedAmount: 0, send(value) {
      acknowledgeNativeInput(session, value);
      if (typeof value !== 'string') {
        const gen = value[1]; samples.set(gen, (samples.get(gen) ?? 0) + (value.length - 5) * 2);
        queueMicrotask(() => session.handleMessage({ t: 'progress', gen, ms: Math.floor(samples.get(gen) / 24) }));
        return;
      }
      const message = JSON.parse(value); events.push(message);
      if (message.t === 'speak') samples.set(message.gen, 0);
      if (message.t === 'text' && message.receipt) queueMicrotask(() => session.handleMessage({ t: 'shown', receipt: message.receipt }));
      if (message.t === 'speak_end' && autoPlayed) queueMicrotask(() => played(message.gen));
      if (message.t === 'speak_cancel' && autoCancelled) queueMicrotask(() => session.handleMessage({ t: 'cancelled', gen: message.gen }));
    } } });
  const played = gen => session.handleMessage({ t: 'played', gen, ms: Math.round((samples.get(gen) ?? 0) / 24) });
  session.start(); t.after(() => session.close());
  const count = kind => events.filter(message => message.t === kind).length;
  const request = (action = 'status', options = {}) => session.printer.request({ t: 'printer', rid: 'a'.repeat(32), action,
    ...(action === 'print' ? { text: 'One copy' } : {}) }, { allowed: () => true, timeoutMs: 500, ...options });
  const reply = (status = 'ready') => session.handleMessage(printerResult('a'.repeat(32), status));
  return { session, engine, events, calls, count, request, reply, played, samples,
    revoke: () => { allowed = false; }, get port() { return port; } };
}

test('every printer action reserves output until its reply, then queued speech resumes', async t => {
  for (const action of ['status', 'print']) await t.test(action, async t => {
    const f = fixture(t); const operation = f.request(action);
    await until(() => f.count('printer') === 1);
    const notification = f.session.notify('An audible notification');
    await sleep(20); assert.equal(f.count('speak'), 0); assert.equal(f.samples.size, 0);
    f.reply(action === 'status' ? 'ready' : 'sent'); await operation;
    await notification; assert.equal(f.count('speak'), 1);
  });
});

test('reservation drains existing playback without cancelling it and rechecks authorization', async t => {
  for (const revoke of [false, true]) await t.test(String(revoke), async t => {
    const f = fixture(t, { autoPlayed: false }); const old = f.session.notify('Already playing');
    await until(() => f.count('speak_end') === 1);
    const gen = f.events.find(message => message.t === 'speak').gen;
    const operation = f.request(); const failure = revoke ? assert.rejects(operation, error => error.code === 'unauthorized') : null;
    await sleep(10); assert.equal(f.count('printer'), 0); assert.equal(f.count('speak_cancel'), 0);
    f.session.handleMessage({ t: 'played', gen: gen + 1, ms: 500 });
    f.session.handleMessage({ t: 'played', gen, ms: 0 });
    await sleep(10); assert.equal(f.count('printer'), 0);
    if (revoke) f.revoke(); f.played(gen); await old;
    if (revoke) { await failure; assert.equal(f.count('printer'), 0); }
    else { await until(() => f.count('printer') === 1); f.reply(); await operation; }
  });
});

test('timeout or abort while waiting for output cannot send a late printer RPC', async t => {
  for (const abort of [false, true]) await t.test(String(abort), async t => {
    const f = fixture(t, { autoPlayed: false }); const old = f.session.notify('Already playing');
    await until(() => f.count('speak_end') === 1);
    const controller = new AbortController();
    const operation = f.request('print', { timeoutMs: abort ? 500 : 15, signal: controller.signal });
    const failure = assert.rejects(operation, error => error.code === (abort ? 'cancelled' : 'timeout') && error.outcome === 'not_sent');
    if (abort) controller.abort(); await failure;
    f.played(f.events.find(message => message.t === 'speak').gen); await old;
    await sleep(20); assert.equal(f.count('printer'), 0);
  });
});

test('post-dispatch abort keeps output and channel busy until a valid late response', async t => {
  const f = fixture(t), controller = new AbortController();
  const operation = f.request('print', { signal: controller.signal });
  const failure = assert.rejects(operation, error => error.code === 'cancelled' && error.outcome === 'unknown');
  await until(() => f.count('printer') === 1); controller.abort(); await failure;
  const notification = f.session.notify('Wait for radio teardown');
  await assert.rejects(f.request(), error => error.code === 'printer_busy');
  f.session.handleMessage(printerResult('b'.repeat(32), 'sent'));
  f.session.handleMessage({ t: 'printer_result', rid: 'a'.repeat(32), status: 'sent' });
  await sleep(20); assert.equal(f.count('speak'), 0);
  f.reply('sent'); await notification;
  assert.equal(f.count('speak'), 1);
  await assert.rejects(operation, error => error.outcome === 'unknown', 'late reply cannot rewrite the caller result');
});

test('a notification queued behind the printer rechecks authorization before output', async t => {
  const f = fixture(t), operation = f.request(); await until(() => f.count('printer') === 1);
  let attempted = 0;
  const notification = f.session.notify('No longer authorized', { beforeAttempt: () => attempted++ });
  const failure = assert.rejects(notification, /disconnected/);
  await sleep(10); f.revoke(); f.reply(); await operation; await failure;
  assert.equal(attempted, 0); assert.equal(f.count('speak'), 0); assert.equal(f.count('text'), 0);
});

test('malformed response does not release output before the bounded remote cleanup guard', async t => {
  const f = fixture(t); f.session.printer = new PrinterChannel(f.session, { cleanupMs: 60 });
  const operation = f.request('print');
  const failure = assert.rejects(operation, error => error.code === 'invalid_response' && error.outcome === 'unknown');
  await until(() => f.count('printer') === 1);
  const notification = f.session.notify('After cleanup');
  f.session.handleMessage({ t: 'printer_result', rid: 'a'.repeat(32), status: 'sent' }); await failure;
  await sleep(15); assert.equal(f.count('speak'), 0);
  await notification; assert.equal(f.count('speak'), 1);
});

test('status caller timeout retains output until valid late cleanup or peer close', async t => {
  const f = fixture(t); const operation = f.request('status', { timeoutMs: 15 });
  const failure = assert.rejects(operation, error => error.code === 'timeout');
  await until(() => f.count('printer') === 1); await failure;
  const notification = f.session.notify('No speech after close');
  const disconnected = assert.rejects(notification, /disconnected/);
  await sleep(15); assert.equal(f.count('speak'), 0);
  f.session.close(); await disconnected; await sleep(5); assert.equal(f.count('speak'), 0);
});

test('Live microphone and replacement PCM survive a printer lease and queued-output barge', async t => {
  const f = fixture(t, { live: true });
  f.session.handleMessage({ t: 'ptt', on: true, turn: 7 }); await f.port.resumeInput();
  const epoch = f.session.epoch, operation = f.request('print');
  await until(() => f.count('printer') === 1);
  f.session.handleAudio({ turn: 7, pcm: Buffer.alloc(1920), reference: Buffer.alloc(1920) });
  assert.equal(f.port.audio(Buffer.alloc(4800, 1)), true); await sleep(10);
  await f.port.cancelReply();
  assert.equal(f.port.audio(Buffer.alloc(9600, 2)), true);
  const finished = f.port.finishReply();
  await sleep(10); assert.equal(f.count('speak'), 0);
  assert.equal(f.calls.input, 1); assert.equal(f.calls.reference, 1);
  assert.equal(f.calls.starts, 1); assert.equal(f.calls.closes, 0); assert.equal(f.session.epoch, epoch);
  f.reply('sent'); await operation; await finished;
  assert.equal(f.count('speak'), 1);
  assert.equal([...f.samples.values()].reduce((sum, value) => sum + value, 0), 4800, 'only replacement PCM reaches device');
  f.session.handleAudio({ turn: 7, pcm: Buffer.alloc(1920) }); assert.equal(f.calls.input, 2);
});

test('Live barge waits for cancellation ACK before printer dispatch without restarting capture', async t => {
  const f = fixture(t, { live: true, autoPlayed: false, autoCancelled: false });
  f.session.handleMessage({ t: 'ptt', on: true, turn: 7 }); await f.port.resumeInput();
  f.port.audio(Buffer.alloc(4800)); await until(() => f.count('speak') === 1);
  const gen = f.events.find(message => message.t === 'speak').gen;
  const operation = f.request(); const cancellation = f.port.cancelReply();
  f.port.audio(Buffer.alloc(4800, 2)); await sleep(10);
  assert.equal(f.count('printer'), 0); assert.equal(f.count('speak'), 1);
  f.session.handleMessage({ t: 'cancelled', gen: gen + 1 }); await sleep(5); assert.equal(f.count('printer'), 0);
  f.session.handleMessage({ t: 'cancelled', gen }); await cancellation;
  await until(() => f.count('printer') === 1); assert.equal(f.count('speak'), 1);
  f.reply(); await operation; await until(() => f.count('speak') === 2);
  assert.equal(f.calls.starts, 1); assert.equal(f.calls.closes, 0);
});

test('Live audio keeps its existing one-minute bound while a PCM segment waits for output ownership', async t => {
  const f = fixture(t, { live: true });
  f.session.handleMessage({ t: 'ptt', on: true, turn: 7 }); await f.port.resumeInput();
  const operation = f.request(); await until(() => f.count('printer') === 1);
  assert.equal(f.port.audio(Buffer.alloc(400 * 48)), true); await sleep(5);
  assert.equal(f.port.audio(Buffer.alloc(59_600 * 48)), true);
  assert.equal(f.port.audio(Buffer.alloc(48)), false, 'held PCM must count toward the same bounded queue');
  await f.port.cancelReply();
  assert.equal(f.port.audio(Buffer.alloc(60_000 * 48)), true, 'cancelling old queued output frees its entire budget');
  await f.port.cancelReply(); f.reply(); await operation;
});

test('lease requires physical playback evidence after an unacknowledged generation', async () => {
  const gate = new SessionOutputLease({ idle: () => true }), abort = new AbortController();
  gate.uncertain(3, 100); let granted = false;
  const operation = gate.acquire(abort.signal).then(release => { granted = true; return release; });
  gate.acknowledge({ t: 'played', gen: 2, ms: 100 }); gate.acknowledge({ t: 'played', gen: 3, ms: 99 });
  await sleep(5); assert.equal(granted, false);
  gate.acknowledge({ t: 'played', gen: 3, ms: 100 }); (await operation)(); gate.close();
});

test('only manual device PTT clears the lost-playback-ACK fence', async () => {
  const gate = new SessionOutputLease({ idle: () => true }), abort = new AbortController();
  gate.uncertain(3, 100); let granted = false;
  const operation = gate.acquire(abort.signal).then(release => { granted = true; return release; });
  gate.acknowledge({ t: 'ptt', on: true, automatic: true }); await sleep(5); assert.equal(granted, false);
  gate.acknowledge({ t: 'ptt', on: true }); (await operation)(); gate.close();
});
