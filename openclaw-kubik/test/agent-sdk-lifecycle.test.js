import assert from 'node:assert/strict';
import test from 'node:test';
import { defineAdapter } from '../src/agent-sdk/index.js';

test('interruption before reply resolves drains the producer without speaking', async () => {
  let release, signal;
  let current = true, completed = false, cleaned = false;
  const spoken = [], errors = [];
  const adapter = defineAdapter({
    id: 'delayed-stream', label: 'Delayed stream', connect() {},
    async reply({ signal: lifetime }) {
      signal = lifetime;
      await new Promise(resolve => { release = resolve; });
      return (async function* () {
        try { yield 'One.'; completed = true; yield 'Two.'; }
        finally { cleaned = true; }
      })();
    },
  });
  await adapter.connect();
  const pending = adapter.dispatch({ isCurrent: () => current,
    speak: async text => spoken.push(text), onAgentError: error => errors.push(error) });
  current = false;
  release();
  await pending;
  assert.equal(signal.aborted, false, 'interrupting speech must let agent work finish');
  assert.equal(completed, true);
  assert.equal(cleaned, true);
  assert.deepEqual(spoken, []);
  assert.deepEqual(errors, []);
  await adapter.close();
});

test('close releases a producer at its next fragment instead of draining it', async () => {
  let release, signal;
  let reads = 0, cleaned = false;
  const spoken = [], errors = [];
  const adapter = defineAdapter({
    id: 'closing-stream', label: 'Closing stream', connect() {},
    async *reply({ signal: lifetime }) {
      signal = lifetime;
      try {
        yield 'One.';
        await new Promise(resolve => { release = resolve; });
        while (true) { reads++; yield ''; }
      } finally { cleaned = true; }
    },
  });
  await adapter.connect();
  const pending = adapter.dispatch({ isCurrent: () => true,
    speak: async text => spoken.push(text), onAgentError: error => errors.push(error) });
  while (!release) await new Promise(resolve => setImmediate(resolve));
  await adapter.close();
  release();
  await pending;
  assert.equal(signal.aborted, true);
  assert.equal(reads, 1);
  assert.equal(cleaned, true);
  assert.deepEqual(spoken, ['One.']);
  assert.deepEqual(errors, []);
});

test('close before reply resolves returns a lazy iterator without starting its next item', async () => {
  let release, nextCalls = 0, returnCalls = 0, producerStarted = false;
  const spoken = [], errors = [];
  const adapter = defineAdapter({
    id: 'late-stream', label: 'Late stream', connect() {},
    async reply() {
      await new Promise(resolve => { release = resolve; });
      const source = (async function* () { producerStarted = true; yield 'Too late.'; })();
      return { [Symbol.asyncIterator]() {
        return {
          next() { nextCalls++; return source.next(); },
          return() { returnCalls++; return source.return(); },
        };
      } };
    },
  });
  await adapter.connect();
  const pending = adapter.dispatch({ isCurrent: () => true,
    speak: async text => spoken.push(text), onAgentError: error => errors.push(error) });
  await adapter.close();
  release();
  await pending;
  assert.equal(nextCalls, 0);
  assert.equal(returnCalls, 1, 'the acquired iterator is closed without asking it for a first item');
  assert.equal(producerStarted, false);
  assert.deepEqual(spoken, []);
  assert.deepEqual(errors, []);
});

test('reply rejection after close is not reported as an agent error', async () => {
  let rejectReply;
  const errors = [];
  const adapter = defineAdapter({ id: 'late-error', label: 'Late error', connect() {},
    reply() { return new Promise((_resolve, reject) => { rejectReply = reject; }); } });
  await adapter.connect();
  const pending = adapter.dispatch({ isCurrent: () => true, speak: async () => {},
    onAgentError: error => errors.push(error.message) });
  await adapter.close();
  rejectReply(new Error('connection closed'));
  await pending;
  assert.deepEqual(errors, []);
});

test('failed connect stays closed until another connect succeeds', async () => {
  let fail = true, calls = 0;
  const signals = [], spoken = [];
  const adapter = defineAdapter({
    id: 'retry-connect', label: 'Retry connect',
    connect({ signal }) { signals.push(signal); if (fail) throw new Error('unavailable'); },
    reply() { calls++; return 'Ready.'; },
  });
  const turn = { isCurrent: () => true, speak: async text => spoken.push(text) };
  await assert.rejects(adapter.connect(), /unavailable/);
  await adapter.dispatch(turn);
  assert.equal(signals[0].aborted, true);
  assert.equal(calls, 0);
  fail = false;
  await adapter.connect();
  await adapter.dispatch(turn);
  assert.equal(signals[1].aborted, false);
  assert.deepEqual(spoken, ['Ready.']);
  await adapter.close();
});
