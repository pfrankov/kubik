import assert from 'node:assert/strict';
import test from 'node:test';
import { fixtureBaseline, restoreFixture, routeRestored } from './fixture-baseline.mjs';

function offline(extra = {}) {
  return { ok: true, device: 'kubik-test', key: 'public-key', character: 'Tess', voice_mode: 'classic',
    ssid: '', url: '', agent: 'OpenClaw', online: false, via: 'none', wifi_connected: false,
    server_pinned: true, guide_done: true, screen_dark: false, menu: false, mic_open: false,
    live_active: false, auto_recording: false, volume: 50, ui_volume: 25, brightness: 200, heap: 123456, ...extra };
}

test('offline fixture is deliberate, complete, and does not mutate saved state', async () => {
  const state = offline(), before = structuredClone(state);
  const result = await fixtureBaseline(async () => state, { identityPath: '/exact/identity.json',
    waitForLink: () => { throw Error('Unexpected normal-route wait'); } });
  assert.equal(result.original, state); assert.equal(result.offlineFixture, true);
  assert.deepEqual(state, before);
});

test('normal, Wi-Fi and already online TLS baselines retain the link wait contract', async () => {
  for (const options of [{ identityPath: undefined }, { wifi: true, identityPath: '/identity' }, { identityPath: '/identity' }]) {
    const state = options.identityPath && !options.wifi ? offline({ online: true, via: 'wifi', wifi_connected: true }) : offline();
    let waited = 0;
    const expected = { online: true, via: 'wifi' };
    const result = await fixtureBaseline(async () => state, { ...options, waitForLink: async () => { waited++; return expected; } });
    assert.equal(waited, 1); assert.deepEqual(result, { original: expected, offlineFixture: false });
  }
  await assert.rejects(fixtureBaseline(async () => offline(), { identityPath: undefined,
    waitForLink: async () => { throw Error('Missing authenticated link'); } }), /Missing authenticated link/);
});

test('offline fixture rejects incomplete, active, configured or ambiguous status', async () => {
  const invalid = [undefined, {}, offline({ key: '' }), offline({ guide_done: undefined }), offline({ volume: 101 }),
    offline({ heap: NaN }), offline({ voice_mode: 'unknown' }), offline({ via: 'usb' }), offline({ wifi_connected: true }), offline({ ssid: 'saved' }),
    offline({ url: 'kubik://saved' }), offline({ mic_open: true }), offline({ live_active: true }), offline({ auto_recording: true })];
  for (const state of invalid)
    await assert.rejects(fixtureBaseline(async () => state, { identityPath: '/identity', waitForLink: () => assert.fail('must reject') }));
  for (const identityPath of ['', null, 3])
    await assert.rejects(fixtureBaseline(async () => offline(), { identityPath }));
});

test('route restoration compares online, selected route and Wi-Fi association', () => {
  const original = offline();
  assert.equal(routeRestored(offline(), original), true);
  for (const change of [{ online: true }, { via: 'usb' }, { wifi_connected: true }])
    assert.equal(routeRestored(offline(change), original), false);
});

test('offline teardown closes before restore; Wi-Fi restores before close even on failure', async () => {
  for (const offlineFixture of [false, true]) {
    for (const fails of [null, 'close', 'restore']) {
      const calls = [];
      const operation = name => async () => { calls.push(name); if (fails === name) throw Error(name); };
      const pending = restoreFixture({ offlineFixture, close: operation('close'), restore: operation('restore') });
      if (fails) await assert.rejects(pending, new RegExp(fails)); else await pending;
      assert.deepEqual(calls, offlineFixture ? ['close', 'restore'] : ['restore', 'close']);
    }
  }
});
