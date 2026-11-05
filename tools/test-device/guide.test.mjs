import assert from 'node:assert/strict';
import test from 'node:test';
import { guideProfileReport, guideProfileVerdict, isProfileScreen, prepareHome } from '../test-device-guide.mjs';

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

const guideFrames = { frames: 90, presented: 90, overflows: 0, fps: 30, late: 0,
  maxMs: 31, gap_us: 35000, deadline_misses: 0 };

test('S3 guide rejects a 50ms presentation gap even at 30fps and zero late frames', () => {
  assert.equal(guideProfileVerdict({ ...guideFrames, gap_us: 50000 }, 'esp32s3'), false);
  assert.equal(guideProfileVerdict(guideFrames, 'esp32s3'), true, '35ms and zero missed deadlines pass');
  assert.equal(guideProfileVerdict({ ...guideFrames, gap_us: 35001 }, 'esp32s3'), false);
  assert.equal(guideProfileVerdict({ ...guideFrames, deadline_misses: 1 }, 'esp32s3'), false);
});

test('S3 guide requires measured nonnegative gaps and integer zero deadline misses', () => {
  for (const key of ['gap_us', 'deadline_misses']) {
    const absent = { ...guideFrames }; delete absent[key];
    assert.equal(guideProfileVerdict(absent, 'esp32s3'), false, `missing ${key}`);
    for (const value of [-1, NaN, Infinity, null, '0'])
      assert.equal(guideProfileVerdict({ ...guideFrames, [key]: value }, 'esp32s3'), false, `${key}=${value}`);
  }
  assert.equal(guideProfileVerdict({ ...guideFrames, deadline_misses: 0.5 }, 'esp32s3'), false);
  assert.equal(guideProfileVerdict({ ...guideFrames, gap_us: 0 }, 'esp32s3'), true);
});

test('guide retains FPS, presentation and overflow requirements for both chips', () => {
  for (const chip of ['esp32c6', 'esp32s3']) {
    assert.equal(guideProfileVerdict({ ...guideFrames, fps: 29.5 }, chip), true);
    for (const patch of [{ fps: 29.49 }, { presented: 89 }, { overflows: 1 },
      { frames: -1, presented: -1 }, { frames: 0, presented: 0 }, { frames: 1.5, presented: 1.5 },
      { presented: -1 }, { late: -1 }, { late: 0.5 }, { overflows: -1 }, { fps: NaN }, { fps: Infinity },
      { maxMs: -1 }, { maxMs: NaN }, { maxMs: Infinity }])
      assert.equal(guideProfileVerdict({ ...guideFrames, ...patch }, chip), false, `${chip}: ${JSON.stringify(patch)}`);
    for (const key of ['frames', 'presented', 'overflows', 'fps', 'late', 'maxMs']) {
      const absent = { ...guideFrames }; delete absent[key];
      assert.equal(guideProfileVerdict(absent, chip), false, `${chip}: missing ${key}`);
    }
  }
});

test('C6 guide keeps its existing budget without imposing S3 presentation metrics', () => {
  const baseline = { ...guideFrames }; delete baseline.gap_us; delete baseline.deadline_misses;
  assert.equal(guideProfileVerdict(baseline, 'esp32c6'), true);
  assert.equal(guideProfileVerdict({ ...baseline, late: 1 }, 'esp32c6'), false);
  assert.equal(guideProfileVerdict({ ...baseline, gap_us: 50000, deadline_misses: 17 }, 'esp32c6'), true);
});

test('guide profile requires an explicit supported chip from original device info', () => {
  for (const chip of [undefined, null, '', 'esp32', 'ESP32-S3'])
    assert.equal(guideProfileVerdict({ ...guideFrames, chip: 'esp32c6' }, chip), false);
  assert.equal(guideProfileVerdict({ ...guideFrames, chip: 'esp32c6', gap_us: 50000 }, 'esp32s3'), false);
  assert.equal(guideProfileVerdict({ ...guideFrames, chip: 'esp32s3', gap_us: 50000 }, 'esp32c6'), true);
  assert.equal(guideProfileVerdict(undefined, 'esp32s3'), false);
});

test('historical iteration vectors remain immutable under the S3 shown-cadence gate', () => {
  // Recorded before asynchronous SPI submission: policy regressions, not current hardware acceptance.
  const vectors = [
    { ...guideFrames, frames: 254, presented: 254, fps: 29.806, gap_us: 33792, late: 8, maxMs: 33.596 },
    { ...guideFrames, gap_us: 33681, late: 2, maxMs: 41.590 },
    { ...guideFrames, fps: 29.868, gap_us: 33618, late: 3, maxMs: 35.6 },
  ];
  for (const vector of vectors) {
    const original = structuredClone(vector), sample = Object.freeze(vector);
    assert.equal(guideProfileVerdict(sample, 'esp32s3'), true);
    assert.equal(guideProfileVerdict(sample, 'esp32c6'), false, 'C6 still requires zero late iterations');
    const report = guideProfileReport(sample, 'esp32s3');
    assert.ok(report.includes(`iteration overruns ${sample.late}, maximum iteration ${sample.maxMs} ms`));
    assert.match(report, /backpressure, TE alignment and first SPI start acknowledgement/);
    assert.match(report, /current DMA tail runs asynchronously/);
    assert.doesNotMatch(report, /zero late|CPU/);
    assert.deepEqual(sample, original, 'raw late, maximum iteration and all other measurements stay unchanged');
  }
});

test('the recorded 50.297ms Guide opening gap and missed deadline remain failures', () => {
  const failed = Object.freeze({ ...guideFrames, gap_us: 50297, deadline_misses: 1, late: 0 });
  const original = structuredClone(failed);
  assert.equal(guideProfileVerdict(failed, 'esp32s3'), false);
  assert.equal(guideProfileVerdict({ ...failed, deadline_misses: 0 }, 'esp32s3'), false);
  assert.equal(guideProfileVerdict({ ...failed, gap_us: 33681 }, 'esp32s3'), false);
  assert.deepEqual(failed, original);
});

test('iteration diagnostics remain mandatory numeric measurements and C6 success wording is unchanged', () => {
  for (const chip of ['esp32c6', 'esp32s3']) {
    for (const patch of [{ late: undefined }, { late: null }, { late: '0' }, { late: Infinity },
      { maxMs: undefined }, { maxMs: null }, { maxMs: '31' }])
      assert.equal(guideProfileVerdict({ ...guideFrames, ...patch }, chip), false);
  }
  assert.equal(guideProfileReport(guideFrames, 'esp32c6'), '30 fps, maximum frame 31 ms, zero late frames');
  assert.throws(() => guideProfileReport(guideFrames, undefined), /Unsupported device chip/);
});
