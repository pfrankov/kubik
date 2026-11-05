import assert from 'node:assert/strict';
import test from 'node:test';
import { restoreNativeFixture } from './native-cleanup.mjs';

function fixture(offlineFixture, failure) {
  const events = [];
  let server = true, listener = true, mode = 'live', closes = 0;
  const step = (name, apply) => async () => {
    events.push(name);
    if (failure === name) throw Error(`${name} failed`);
    apply?.();
  };
  const callbacks = { offlineFixture,
    cancelCapture: step('capture'),
    restoreMode: step('mode', () => {
      assert.equal(server, true, 'Original capabilities need a live authenticated fixture session');
      mode = 'classic';
    }),
    stopServer: step('stop', () => { server = false; }),
    waitForStatus: step('status', () => assert.equal(server, false)),
    restoreRoute: step('route', () => {
      assert.equal(server, false);
      if (offlineFixture) assert.equal(listener, false, 'Offline baseline cannot still connect to the fixture');
      else { assert.equal(listener, true); mode = 'classic'; } // the original host supplies its capabilities
    }),
    close: step('close', () => { closes++; listener = false; }) };
  return { callbacks, events, mode: () => mode, server: () => server, listener: () => listener, closes: () => closes };
}

test('offline native cleanup applies original mode before losing the only capability session', async () => {
  const item = fixture(true);
  await restoreNativeFixture(item.callbacks);
  assert.deepEqual(item.events, ['capture', 'mode', 'stop', 'status', 'capture', 'close', 'route']);
  assert.equal(item.mode(), 'classic'); assert.equal(item.server(), false); assert.equal(item.listener(), false);
  assert.equal(item.closes(), 1);
});

test('normal online fixture releases mock before the primary route supplies its mode again', async () => {
  const item = fixture(false);
  await restoreNativeFixture(item.callbacks);
  assert.deepEqual(item.events, ['stop', 'status', 'capture', 'route', 'close']);
  assert.equal(item.mode(), 'classic'); assert.equal(item.closes(), 1);
});

test('capability restoration failure still stops server and closes listener once', async () => {
  const item = fixture(true, 'mode');
  await assert.rejects(restoreNativeFixture(item.callbacks), /mode failed/);
  assert.deepEqual(item.events, ['capture', 'mode', 'stop', 'close']);
  assert.equal(item.server(), false); assert.equal(item.listener(), false); assert.equal(item.closes(), 1);
});

test('missing complete status blocks endpoint writes but still closes the transport', async () => {
  const item = fixture(false, 'status');
  await assert.rejects(restoreNativeFixture(item.callbacks), /status failed/);
  assert.deepEqual(item.events, ['stop', 'status', 'close']);
  assert.equal(item.listener(), false); assert.equal(item.closes(), 1);
});

test('failed route restoration cannot retain the fixture listener', async () => {
  for (const offline of [false, true]) {
    const item = fixture(offline, 'route');
    await assert.rejects(restoreNativeFixture(item.callbacks), /route failed/);
    assert.equal(item.server(), false); assert.equal(item.listener(), false); assert.equal(item.closes(), 1);
  }
});
