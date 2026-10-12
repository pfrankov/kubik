import assert from 'node:assert/strict';
import test from 'node:test';
import { LiveVoiceSession } from '../src/live-voice.js';
import { NativeVoiceTurn } from '../src/native-voice.js';

const settle = () => new Promise(resolve => setImmediate(resolve));

function pendingVoice(Voice, options = {}) {
  let connected;
  const connecting = new Promise(resolve => { connected = resolve; });
  const sent = [];
  let connects = 0, closes = 0, failures = 0;
  const turn = new Voice({ config: {}, dispatch: async () => {}, turnId: 7, ...options,
    createBridge: () => ({ bridge: { pacesInputAudio: false }, connect: () => { connects++; return connecting; },
      sendAudio: pcm => sent.push(pcm), close: () => { closes++; } }),
    port: { isCurrent: () => true, resumeInput: async () => {},
      endInput() {}, fail: () => { failures++; } } });
  return { turn, sent, connected, get connects() { return connects; },
    get closes() { return closes; }, get failures() { return failures; } };
}

test('cancellation before the connection microtask does not start a provider', async () => {
  for (const Voice of [NativeVoiceTurn, LiveVoiceSession]) {
    const fixture = pendingVoice(Voice);
    fixture.turn.close();
    await settle();
    assert.equal(fixture.connects, 0, `${Voice.name} started a provider after cancellation`);
    assert.equal(fixture.closes, 1);
    assert.equal(fixture.failures, 0);
    fixture.connected(); await settle();
    assert.equal(fixture.sent.length, 0);
  }
});

test('closing pending Realtime and Live releases all connection and session timers', async t => {
  const schedule = setTimeout, cancel = clearTimeout, active = new Set();
  t.mock.method(globalThis, 'setTimeout', (...args) => {
    const timer = schedule(...args); active.add(timer); return timer;
  });
  t.mock.method(globalThis, 'clearTimeout', timer => { active.delete(timer); return cancel(timer); });
  t.after(() => { for (const timer of active) cancel(timer); });
  for (const Voice of [NativeVoiceTurn, LiveVoiceSession]) {
    const fixture = pendingVoice(Voice);
    await settle();
    assert.equal(active.size, 2, 'connection and conversation deadlines are armed');
    fixture.turn.append(Buffer.alloc(1920), Buffer.alloc(1920));
    fixture.turn.close(); fixture.turn.close();
    assert.equal(active.size, 0, `${Voice.name} retained a timer after close`);
    await settle();
    assert.equal(fixture.closes, 1);
    fixture.connected(); await settle();
    assert.equal(fixture.sent.length, 0, 'late connection completion sent cancelled input');
    assert.equal(fixture.failures, 0);
  }
});

test('connection timeout still fails once and late completion cannot flush queued input', async () => {
  for (const Voice of [NativeVoiceTurn, LiveVoiceSession]) {
    const fixture = pendingVoice(Voice, { connectMs: 10, turnMs: 1000, sessionMs: 1000 });
    fixture.turn.append(Buffer.alloc(1920), Buffer.alloc(1920));
    await new Promise(resolve => setTimeout(resolve, 30));
    assert.equal(fixture.connects, 1);
    assert.equal(fixture.failures, 1); assert.equal(fixture.closes, 1);
    fixture.connected(); await settle();
    assert.equal(fixture.sent.length, 0);
    fixture.turn.close(); await settle();
    assert.equal(fixture.failures, 1); assert.equal(fixture.closes, 1);
  }
});

test('Live connection queue retains owned echo output without a second PCM allocation', async t => {
  const fixture = pendingVoice(LiveVoiceSession);
  t.after(() => fixture.turn.close());
  await settle();
  const pcm = Buffer.alloc(1920), reference = Buffer.alloc(1920), owned = [];
  const allocate = Buffer.allocUnsafe, from = Buffer.from;
  let copies = 0;
  t.mock.method(Buffer, 'allocUnsafe', size => { const data = allocate(size); owned.push(data); return data; });
  t.mock.method(Buffer, 'from', (...args) => { copies++; return from(...args); });
  fixture.turn.append(pcm, reference);
  pcm.fill(127); reference.fill(127); // capture callers may reuse their buffers
  fixture.turn.append(Buffer.alloc(1920), Buffer.alloc(1920));
  t.mock.restoreAll();
  assert.equal(owned.length, 2, 'echo cancellation allocates one result per input frame');
  assert.equal(copies, 0, 'queued echo output was copied again');
  fixture.connected(); await settle();
  assert.equal(fixture.sent.length, 2);
  assert.equal(fixture.sent[0], owned[0]); assert.equal(fixture.sent[1], owned[1]);
  assert.notEqual(owned[0], owned[1]);
  assert.ok(fixture.sent.every(data => data.every(byte => byte === 0)), 'queued silence changed with capture scratch');
  fixture.turn.close(); await settle();
  assert.equal(fixture.closes, 1); assert.equal(fixture.failures, 0);
});
