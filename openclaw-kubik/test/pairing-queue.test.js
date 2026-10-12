import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import test from 'node:test';
import { guardSessionFrames } from '../src/pairing-access.js';
import { parseAudioFrame } from '../src/protocol.js';

const settle = () => new Promise(resolve => setImmediate(resolve));

function waitingFrames() {
  const ws = Object.assign(new EventEmitter(), { readyState: 1 });
  const conn = { ws, phase: 'session' }, delivered = [];
  let authorize, failures = 0;
  const allowed = new Promise(resolve => { authorize = resolve; });
  const session = { ws, closed: false, handleMessage: value => delivered.push(value),
    handleAudio: value => delivered.push(value) };
  const close = () => { ws.readyState = 3; ws.emit('close'); };
  const frames = guardSessionFrames(session, conn, { authorize: () => allowed,
    onFailure: () => { failures++; conn.phase = 'closed'; close(); },
    onFrameError: error => { throw error; } });
  frames.message({ t: 'ptt', on: true, turn: 7 });
  return { frames, close, delivered, authorize, get failures() { return failures; } };
}

function audio(duplex) {
  const wire = Buffer.alloc(duplex ? 968 : 485);
  wire[0] = duplex ? 5 : 4; wire[1] = 7;
  return parseAudioFrame(wire);
}

test('authorization queue counts both decoded Live channels at the 64 KiB boundary', async () => {
  for (const duplex of [false, true]) {
    for (const overflow of [false, true]) {
      const fixture = waitingFrames();
      const first = audio(duplex);
      const bytes = first.pcm.length + (first.reference?.length ?? 0);
      const capacity = Math.floor(64 * 1024 / bytes);
      await settle();
      for (let i = 0; i < capacity; i++) fixture.frames.audio(i ? audio(duplex) : first);
      assert.equal(fixture.failures, 0);
      assert.equal(fixture.delivered.length, 0, 'audio bypassed pending authorization');
      if (overflow) {
        fixture.frames.audio(audio(duplex));
        assert.equal(fixture.failures, 1, `retained more than 64 KiB (${duplex ? 'Live' : 'mono'})`);
      }
      fixture.authorize(true); await settle();
      assert.equal(fixture.delivered.length, overflow ? 0 : capacity + 1);
      if (!overflow) {
        assert.equal(fixture.delivered[0].t, 'ptt');
        assert.equal(fixture.delivered[1], first, 'queued frame ownership/order changed');
      }
    }
  }
});

test('small authorized frames still use the 64-frame limit and close releases pending input', async () => {
  for (const close of [false, true]) {
    const fixture = waitingFrames();
    await settle();
    for (let i = 0; i < 64; i++) fixture.frames.message({ t: 'ping', ts: i });
    assert.equal(fixture.failures, 0);
    if (close) fixture.close();
    else {
      fixture.frames.message({ t: 'ping', ts: 64 });
      assert.equal(fixture.failures, 1);
    }
    fixture.authorize(true); await settle();
    assert.equal(fixture.delivered.length, 0, 'close must discard queued and in-flight delivery');
  }
});
