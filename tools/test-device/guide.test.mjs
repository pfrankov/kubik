import assert from 'node:assert/strict';
import test from 'node:test';
import { isProfileScreen, prepareHome } from '../test-device-guide.mjs';

const home = { screen_dark: false, menu: false, agent_open: false };

test('guide replay closes reboot setup with BOOT while preserving an unconfigured network', async () => {
  let state = { ...home, setup_ap: 'Kubik-TEST', ssid: '', url: '', key: 'unchanged' };
  const events = [];
  const result = await prepareHome({ read: async () => state,
    event: async event => {
      events.push(event); assert.equal(event, 'boot');
      state = { ...state }; delete state.setup_ap;
    },
    wait: async (check, label) => { assert.ok(check(state), label); return state; } });
  assert.deepEqual(events, ['boot']);
  assert.equal(result.ssid, ''); assert.equal(result.url, ''); assert.equal(result.key, 'unchanged');
  assert.equal(isProfileScreen(result, 'home'), true);
});

test('guide preflight wakes before BOOT and leaves Settings only after setup is gone', async () => {
  let state = { ...home, screen_dark: true, setup_ap: 'Kubik-TEST', menu: true, agent_open: true };
  const events = [];
  await prepareHome({ read: async () => state,
    event: async event => {
      events.push(event);
      if (event === 'pwr') state.screen_dark = false;
      if (event === 'boot') delete state.setup_ap;
      if (event === 'menu') { state.menu = false; state.agent_open = false; }
    }, wait: async (check, label) => { assert.ok(check(state), label); return state; } });
  assert.deepEqual(events, ['pwr', 'boot', 'menu']);
  events.length = 0;
  await prepareHome({ read: async () => state, event: async event => events.push(event),
    wait: async (check, label) => { assert.ok(check(state), label); return state; } });
  assert.deepEqual(events, [], 'Home must not be toggled into Settings');
});

test('a lost BOOT is a preflight failure instead of a successful QR frame sample', async () => {
  const state = { ...home, setup_ap: 'Kubik-TEST', online: true, guide_done: true };
  await assert.rejects(prepareHome({ read: async () => state, event: async () => {},
    wait: async (check, label) => { assert.ok(check(state), label); return state; } }), /BOOT closes initial setup/);
  for (const view of ['home', 'settings', 'guide']) assert.equal(isProfileScreen(state, view), false);
});

test('frame preconditions distinguish Settings, guide steps and overlays', () => {
  for (const view of ['home', 'settings', 'guide']) assert.equal(isProfileScreen({}, view), false);
  const settings = { ...home, menu: true }, guide = { ...settings, agent_open: true, agent_view: 'guide', guide_step: 0 };
  assert.equal(isProfileScreen(home, 'home'), true);
  assert.equal(isProfileScreen(settings, 'settings'), true);
  assert.equal(isProfileScreen(guide, 'guide', 0), true);
  assert.equal(isProfileScreen(settings, 'guide'), false);
  assert.equal(isProfileScreen(guide, 'settings'), false);
  assert.equal(isProfileScreen(guide, 'guide', 1), false);
  assert.equal(isProfileScreen({ ...guide, agent_view: 'overview' }, 'guide'), false);
  for (const key of ['screen_dark', 'setup_ap', 'screen_lab', 'status_open', 'sound_menu', 'card_open', 'pair_visible']) {
    for (const [state, view] of [[home, 'home'], [settings, 'settings'], [guide, 'guide']])
      assert.equal(isProfileScreen({ ...state, [key]: true }, view), false, `${view}: ${key}`);
  }
});
