// An offline device may use an explicit TLS USB fixture without changing saved networks.
import assert from 'node:assert/strict';
import { waitForDeviceLink } from './diagnostics.mjs';

const strings = ['device', 'key', 'character', 'voice_mode', 'ssid', 'url', 'agent'];
const flags = ['online', 'wifi_connected', 'server_pinned', 'guide_done', 'screen_dark', 'menu',
  'mic_open', 'live_active', 'auto_recording'];
const numbers = { volume: [0, 100], ui_volume: [0, 100], brightness: [10, 255], heap: [0, Number.MAX_SAFE_INTEGER] };

function requireOfflineSnapshot(state) {
  assert.equal(state?.ok, true, 'Complete device status is required');
  for (const key of strings) assert.equal(typeof state[key], 'string', `Missing device status: ${key}`);
  for (const key of ['device', 'key', 'voice_mode']) assert.ok(state[key].length, `Missing device status: ${key}`);
  assert.ok(['Tess', 'Plush'].includes(state.character), 'Invalid device character');
  assert.ok(['classic', 'realtime', 'live'].includes(state.voice_mode), 'Invalid device voice mode');
  for (const key of flags) assert.equal(typeof state[key], 'boolean', `Missing device status: ${key}`);
  for (const [key, [low, high]] of Object.entries(numbers))
    assert.ok(Number.isSafeInteger(state[key]) && state[key] >= low && state[key] <= high, `Invalid device status: ${key}`);
  assert.equal(state.online, false, 'Offline USB fixture requires an offline device');
  assert.equal(state.via, 'none', 'Offline USB fixture requires no active route');
  for (const key of ['mic_open', 'live_active', 'auto_recording']) assert.equal(state[key], false, `Device is active: ${key}`);
}

export async function fixtureBaseline(read, { wifi = false, identityPath = process.env.KUBIK_TEST_TLS_IDENTITY,
  waitForLink = waitForDeviceLink } = {}) {
  if (wifi || identityPath === undefined) return { original: await waitForLink(read), offlineFixture: false };
  assert.ok(typeof identityPath === 'string' && identityPath.length, 'TLS USB fixture requires an explicit identity file');
  const state = await read();
  if (state?.online) return { original: await waitForLink(read), offlineFixture: false };
  requireOfflineSnapshot(state);
  return { original: state, offlineFixture: true };
}

export function routeRestored(state, original) {
  if (original.online === false && original.via === 'none') {
    if (typeof state.wifi_connected !== 'boolean') return false;
    return state.online === false && state.via === 'none' ||
      state.online === true && state.via === 'wifi' && state.wifi_connected;
  }
  return state.online === original.online && state.via === original.via &&
    state.wifi_connected === original.wifi_connected;
}

// During an outage firmware rotates existing profiles in RAM. Status reports
// that current candidate, not the stored profile set; these fixtures never write it.
export function assertSettingRestored(state, original, key) {
  if (key === 'ssid' && original.online === false) {
    assert.equal(typeof state.ssid, 'string', 'Missing current Wi-Fi candidate');
    assert.ok(Buffer.byteLength(state.ssid) <= 32, 'Invalid current Wi-Fi candidate');
    return;
  }
  assert.equal(state[key], original[key], `${key} changed`);
}

// A Wi-Fi baseline needs the mock until its route resumes. An offline baseline
// must release the mock first, whether its normal network stays down or recovers.
export async function restoreFixture({ offlineFixture, close, restore }) {
  if (offlineFixture) {
    try { await close(); } finally { await restore(); }
  } else {
    try { await restore(); } finally { await close(); }
  }
}
