import test from 'node:test';
import assert from 'node:assert/strict';
import { SerialWriter, SERIAL_QUEUE_BYTES } from './serial-writer.mjs';

function fixture() {
  let now = 0, next = 0;
  const timers = new Map(), writes = [], errors = [], callbacks = [];
  const clock = { setTimer: (fn, ms) => { const id = ++next; timers.set(id, { at: now + ms, fn }); return id; },
    clearTimer: id => timers.delete(id) };
  const tick = ms => {
    const end = now + ms;
    for (;;) {
      const first = [...timers].sort((a, b) => a[1].at - b[1].at)[0];
      if (!first || first[1].at > end) break;
      timers.delete(first[0]); now = first[1].at; first[1].fn();
    }
    now = end;
  };
  const port = { write: (data, cb) => { writes.push({ at: now, data: Buffer.from(data) }); callbacks.push(cb); },
    drain: cb => callbacks.push(cb) };
  const writer = new SerialWriter(port, reason => errors.push(reason), clock);
  return { writer, port, tick, writes, errors, callbacks, timers };
}

test('USB bursts retain exact framing/order, with one bounded chunk per drain and pause', () => {
  const s = fixture(), frames = [Buffer.alloc(1211, 1), Buffer.alloc(1211, 2), Buffer.alloc(4096, 3)];
  for (const frame of frames) assert.equal(s.writer.send(frame), true);
  assert.equal(s.writes.length, 1);
  s.tick(400); assert.equal(s.writes.length, 1, 'write callback still pending');
  while (s.writer.bytes) {
    const before = s.writes.length;
    s.callbacks.shift()(); // write completed, OS drain still pending
    s.tick(1); assert.equal(s.writes.length, before);
    s.callbacks.shift()(); // drain completed
    s.tick(4); assert.equal(s.writes.length, before);
    s.tick(1);
  }
  s.tick(5);
  assert.deepEqual(Buffer.concat(s.writes.map(w => w.data)), Buffer.concat(frames));
  assert.ok(s.writes.every(w => w.data.length <= 512));
  assert.ok(s.writes.slice(1).every((w, i) => w.at - s.writes[i].at >= 5));
  assert.equal(s.timers.size, 0); assert.deepEqual(s.errors, []);
});

test('full queues, stalled writes and drain failures fail once and discard pending bytes', () => {
  for (const phase of ['overflow', 'write', 'drain', 'drain-error', 'throw']) {
    const s = fixture();
    if (phase === 'throw') s.port.write = () => { throw Error('private driver detail'); };
    s.writer.send(Buffer.alloc(SERIAL_QUEUE_BYTES));
    if (phase === 'overflow') assert.equal(s.writer.send(Buffer.alloc(1)), false);
    if (phase.startsWith('drain')) s.callbacks.shift()();
    if (phase === 'drain-error') s.callbacks.shift()(Error('private driver detail'));
    s.tick(1000);
    for (const callback of s.callbacks.splice(0)) callback(); // late completion after failure
    s.tick(2000);
    assert.equal(s.errors.length, 1, phase);
    assert.doesNotMatch(s.errors[0], /private/);
    assert.equal(s.writer.bytes, 0); assert.equal(s.timers.size, 0);
    assert.equal(s.writer.send(Buffer.alloc(1)), false);
  }
});

test('closing during write, drain or pacing prevents queued or late writes to a replaced port', () => {
  for (const phase of ['write', 'drain', 'pause']) {
    const s = fixture(); s.writer.send(Buffer.alloc(3000));
    if (phase !== 'write') s.callbacks.shift()();
    if (phase === 'pause') s.callbacks.shift()();
    s.writer.close();
    for (const callback of s.callbacks.splice(0)) callback();
    s.tick(5000);
    assert.equal(s.writes.length, 1); assert.equal(s.writer.bytes, 0);
    assert.equal(s.timers.size, 0); assert.deepEqual(s.errors, []);
  }
});
