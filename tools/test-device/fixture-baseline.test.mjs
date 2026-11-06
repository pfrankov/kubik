import assert from 'node:assert/strict';
import test from 'node:test';
import { assertSettingRestored, fixtureBaseline, pinnedFixture, restoreFixture, routeRestored } from './fixture-baseline.mjs';
import { restoreNotificationFixture } from '../test-device-notify.mjs';
import { restorePairingFixture } from '../test-device-pairing.mjs';

function offline(extra = {}) {
  return { ok: true, device: 'kubik-test', key: 'public-key', character: 'Tess', voice_mode: 'classic',
    ssid: '', url: '', agent: 'OpenClaw', online: false, via: 'none', wifi_connected: false,
    server_pinned: true, guide_done: true, screen_dark: false, menu: false, mic_open: false,
    live_active: false, auto_recording: false, volume: 50, ui_volume: 25, brightness: 200, heap: 123456, ...extra };
}

test('offline fixture is deliberate, complete, and does not mutate saved state', async () => {
  for (const configuration of [{}, { ssid: 'saved Wi-Fi', url: 'kubik://saved.example:18793' }]) {
    for (const wifi_connected of [false, true]) {
      const state = offline({ ...configuration, wifi_connected }), before = structuredClone(state);
      const result = await fixtureBaseline(async () => state, { identityPath: '/exact/identity.json',
        waitForLink: () => { throw Error('Unexpected normal-route wait'); } });
      assert.equal(result.original, state); assert.equal(result.offlineFixture, true);
      assert.deepEqual(state, before);
    }
  }
});

test('explicit pinned Wi-Fi accepts only the exact disconnected saved endpoint and identity', async () => {
  const pinnedEndpoint = 'kubik://n8n.frankova.ru:18794';
  const state = offline({ url: pinnedEndpoint, wifi_connected: true });
  const options = { wifi: true, identityPath: '/existing/identity.json', pinnedEndpoint,
    waitForLink: () => assert.fail('Production was deliberately stopped') };
  assert.deepEqual(await fixtureBaseline(async () => state, options), { original: state, offlineFixture: true });
  assert.deepEqual(pinnedFixture(state, options), { port: 18794, identityPath: options.identityPath });
  for (const changed of [{ url: 'kubik://other:18794' }, { url: 'kubik://n8n.frankova.ru:18793' },
    { server_pinned: false }, { online: true, via: 'wifi' }, { via: 'usb' }, { mic_open: true }])
    await assert.rejects(fixtureBaseline(async () => ({ ...state, ...changed }), options));
  for (const changed of [{ wifi: false }, { identityPath: '' }, { pinnedEndpoint: '' },
    { pinnedEndpoint: 'kubik://host' }, { pinnedEndpoint: 'kubik://host:0' },
    { pinnedEndpoint: 'kubik://user:secret@host:18794' }, { pinnedEndpoint: 'kubik://host:18794/path' },
    { pinnedEndpoint: 'wss://host:18794' }])
    await assert.rejects(fixtureBaseline(async () => state, { ...options, ...changed }));
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

test('offline fixture rejects incomplete, active or ambiguous status', async () => {
  const invalid = [undefined, {}, offline({ key: '' }), offline({ guide_done: undefined }), offline({ volume: 101 }),
    offline({ heap: NaN }), offline({ voice_mode: 'unknown' }), offline({ via: 'usb' }), offline({ wifi_connected: undefined }),
    offline({ ssid: undefined }), offline({ url: undefined }), offline({ mic_open: true }), offline({ live_active: true }), offline({ auto_recording: true })];
  for (const state of invalid)
    await assert.rejects(fixtureBaseline(async () => state, { identityPath: '/identity', waitForLink: () => assert.fail('must reject') }));
  for (const identityPath of ['', null, 3])
    await assert.rejects(fixtureBaseline(async () => offline(), { identityPath }));
});

test('online baseline still requires the exact active route and Wi-Fi association', () => {
  const original = offline({ online: true, via: 'wifi', wifi_connected: true });
  assert.equal(routeRestored({ ...original }, original), true);
  for (const change of [{ online: false }, { via: 'usb' }, { wifi_connected: false }])
    assert.equal(routeRestored({ ...original, ...change }, original), false);
});

test('offline teardown accepts a continued outage or recovered normal Wi-Fi but never USB', () => {
  const original = offline({ ssid: 'saved Wi-Fi', url: 'kubik://saved.example:18793' });
  for (const wifi_connected of [false, true])
    assert.equal(routeRestored(offline({ wifi_connected }), original), true);
  assert.equal(routeRestored(offline({ online: true, via: 'wifi', wifi_connected: true }), original), true);
  for (const change of [{ online: true }, { via: 'usb' }, { via: 'wifi' }, { wifi_connected: undefined },
    { online: true, via: 'wifi', wifi_connected: false }, { online: true, via: 'usb', wifi_connected: true }])
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

function savedFixture(recovered, changed = {}) {
  const original = offline({ ssid: 'saved network', url: 'kubik://saved.example:18793',
    agent: 'Hermes', screen_dark: true });
  const state = { ...original, online: true, via: 'usb', screen_dark: false, pair_visible: false };
  const commands = [];
  let closed = false;
  const request = async command => {
    commands.push(command);
    if (command.cmd === 'info') return { ...state };
    if (command.cmd === 'card' && command.text === '') return { ok: true };
    assert.equal(command.cmd, 'sim', 'Fixture cleanup must never persist device settings');
    if (command.ev === 'wifi') {
      assert.equal(closed, true, 'Close offline TLS listener before resuming the original network');
      assert.equal(command.ms, 1);
      Object.assign(state, recovered ? { online: true, via: 'wifi', wifi_connected: true } :
        { online: false, via: 'none', wifi_connected: false }, changed);
    } else {
      assert.equal(command.ev, 'pwr'); state.screen_dark = !state.screen_dark;
    }
    return { ok: true };
  };
  const hooks = { request, read: () => request({ cmd: 'info' }),
    simulate: (ev, extra) => request({ cmd: 'sim', ev, ...extra }),
    wait: async check => assert.equal(await check({ ...state }), true, 'Restoration condition was not satisfied') };
  return { original, state, commands, hooks, close: async () => { closed = true; state.online = false; state.via = 'none'; } };
}

test('notification and pairing cleanup preserve configured offline state without NVS writes', async () => {
  for (const restore of [restoreNotificationFixture, restorePairingFixture]) {
    for (const recovered of [false, true]) {
      const fixture = savedFixture(recovered);
      await restoreFixture({ offlineFixture: true, close: fixture.close,
        restore: () => restore(fixture.original, fixture.hooks) });
      assert.equal(fixture.state.screen_dark, true);
      assert.equal(routeRestored(fixture.state, fixture.original), true);
      for (const field of ['device', 'agent', 'key', 'ssid', 'url', 'server_pinned', 'guide_done'])
        assert.equal(fixture.state[field], fixture.original[field]);
      assert.equal(fixture.commands.filter(command => command.ev === 'wifi').length, 1);
    }
  }
});

test('shared cleanup rejects retained USB, inconsistent Wi-Fi or changed saved identity', async () => {
  for (const restore of [restoreNotificationFixture, restorePairingFixture]) {
    for (const changed of [{ online: true, via: 'usb' }, { online: true, via: 'wifi', wifi_connected: false },
      { device: 'replacement' }, { agent: 'other' }, { key: 'other' }, { ssid: undefined }, { url: 'kubik://other' }]) {
      const fixture = savedFixture(false, changed);
      await assert.rejects(restoreFixture({ offlineFixture: true, close: fixture.close,
        restore: () => restore(fixture.original, fixture.hooks) }));
    }
  }
});

test('offline profile rotation is observed without writing stored networks; online SSID stays exact', async () => {
  for (const restore of [restoreNotificationFixture, restorePairingFixture]) {
    const fixture = savedFixture(false, { ssid: 'another saved candidate' });
    await restoreFixture({ offlineFixture: true, close: fixture.close,
      restore: () => restore(fixture.original, fixture.hooks) });
    assert.ok(fixture.commands.every(command => command.cmd !== 'set'));
  }
  const original = offline({ online: true, via: 'wifi', wifi_connected: true, ssid: 'saved' });
  assert.throws(() => assertSettingRestored({ ...original, ssid: 'other' }, original, 'ssid'));
  assert.throws(() => assertSettingRestored({ ssid: 'a'.repeat(33) }, offline(), 'ssid'));
});
