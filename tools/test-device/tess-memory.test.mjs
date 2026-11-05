import assert from 'node:assert/strict';
import test from 'node:test';
import { TessContinuity, repeatScatter } from './tess-memory.mjs';

const info = (uptime_ms = 10000) => ({uptime_ms,heap_failures:0});
const bridge = () => ({ok:true,instance:'test-bridge',faults:0,boots:1,disconnects:0});
test('continuity accepts elapsed uptime and unchanged bridge fault history', () => {
  const proof = new TessContinuity(info(), bridge(), 1000);
  proof.check(info(11000), bridge(), 2000);
  proof.check(info(15000), bridge(), 6000);
});
test('continuity rejects uptime reset, stalled uptime and allocation failure', () => {
  for (const state of [info(100), info(10000), {...info(15000),heap_failures:1}]) {
    const proof = new TessContinuity(info(), bridge(), 1000);
    assert.throws(() => proof.check(state, bridge(), 6000));
  }
});
test('continuity rejects panic, reboot, disconnect and reset bridge counters', () => {
  for (const changed of [{faults:1},{boots:2},{disconnects:1},{instance:'replacement'}]) {
    const proof = new TessContinuity(info(), bridge(), 1000);
    assert.throws(() => proof.check(info(15000), {...bridge(),...changed}, 6000));
  }
  assert.throws(() => new TessContinuity(info(), {...bridge(),faults:1}, 1000));
});
test('each bounded rapid-tap round must observe scatter and its actual return', async () => {
  let taps = 0, checks = 0, prepared = 0, returned = 0;
  const transitions = [];
  await repeatScatter({
    prepare:async cycle => assert.equal(cycle, prepared++),
    tap:async () => {taps++;}, check:async () => {checks++;}, pause:async () => {},
    game:async () => ({preview:true,burst:taps > returned * 6 ? 1 : 0,game:0}),
    wait:async (read, predicate, label) => {
      if (label.includes('return')) returned++;
      assert.ok(predicate(await read()), label); transitions.push(label);
    },
  });
  assert.equal(taps, 24); assert.equal(checks, 32); assert.equal(prepared, 4);
  assert.equal(returned, 4); assert.equal(transitions.length, 8);
});
test('a fixture that never scatters fails instead of claiming the stack path was exercised', async () => {
  await assert.rejects(repeatScatter({cycles:1, tap:async () => {}, check:async () => {}, pause:async () => {},
    game:async () => ({preview:true,burst:0,game:0}),
    wait:async (read, predicate, label) => assert.ok(predicate(await read()),label),
  }), /real taps scatter/);
});
test('the physical repro cannot request an unbounded number of rounds', async () => {
  for (const cycles of [0,9,Infinity]) await assert.rejects(repeatScatter({cycles}));
});
