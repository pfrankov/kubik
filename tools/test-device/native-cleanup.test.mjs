import assert from 'node:assert/strict';
import test from 'node:test';
import { cleanupPreservingFailure, restoreNativeFixture, restoreNativeScreen } from './native-cleanup.mjs';

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

function screenFixture(original, current, ignoreMenu = false) {
  const state = { ...current }, commands = [], checks = [];
  const info = async () => ({ ...state });
  const sim = async event => {
    commands.push(event);
    if (event === 'pwr') state.screen_dark = !state.screen_dark;
    else if (!state.screen_dark && !ignoreMenu) state.menu = !state.menu;
  };
  const until = async (predicate, label) => {
    checks.push(label);
    const fresh = await info();
    assert.ok(predicate(fresh), label);
    return fresh;
  };
  return { state, commands, checks, run: () => restoreNativeScreen(original, { info, sim, until }) };
}

test('screen restoration verifies all dark/menu combinations before reporting success', async () => {
  for (const screen_dark of [false, true]) {
    for (const menu of [false, true]) {
      const original = { screen_dark, menu };
      for (const current of [{ screen_dark: false, menu: false }, { screen_dark: true, menu: !menu }]) {
        const fixture = screenFixture(original, current);
        assert.deepEqual(await fixture.run(), original);
        assert.deepEqual(fixture.state, original);
        assert.equal(fixture.checks.at(-1), 'original screen and menu restored');
      }
    }
  }
  const regression = screenFixture({ screen_dark: true, menu: true }, { screen_dark: false, menu: false });
  await regression.run();
  assert.deepEqual(regression.commands, ['menu', 'pwr']);
});

test('an ignored menu input fails restoration instead of trusting the command acknowledgement', async () => {
  const fixture = screenFixture({ screen_dark: true, menu: true }, { screen_dark: false, menu: false }, true);
  await assert.rejects(fixture.run(), /original menu restored/);
  assert.deepEqual(fixture.commands, ['menu']);
});

test('cleanup failures remain failures and never replace the original playback error', async () => {
  const primary = Error('Speaker gaps: 1'), secondary = Error('Menu was not restored'), reported = [];
  const cleanup = async () => { throw secondary; };
  await assert.rejects(cleanupPreservingFailure(undefined, cleanup), error => error === secondary);
  const operation = async () => {
    let failure;
    try { throw primary; }
    catch (error) { failure = error; throw error; }
    finally { await cleanupPreservingFailure(failure, cleanup, error => reported.push(error)); }
  };
  await assert.rejects(operation(), error => error === primary);
  assert.deepEqual(reported, [secondary]);
});
