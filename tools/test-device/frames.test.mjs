import assert from 'node:assert/strict';
import test from 'node:test';
import { frameVerdict, parseStats } from './frames.mjs';

test('physical baseline remains accepted only in its explicit transient category', () => {
  // Original app+assets, same corrected physical fixture, 2026-10-02; no regenerated frame goldens.
  const onset = { fps: 27.6, frames: 12, meanMs: 30.3, maxMs: 39.6, late: 6, gapMs: 51.2 };
  assert.equal(frameVerdict(onset, 'onset'), true);
  assert.equal(frameVerdict(onset), false);
  assert.equal(frameVerdict({ ...onset, late: 7 }, 'onset'), false);
  assert.equal(frameVerdict({ ...onset, frames: 7 }, 'onset'), false);
  for (const changed of [{ maxMs: 41.001 }, { gapMs: 56.001 }, { meanMs: 33.334 }, { frames: 0 }]) {
    assert.equal(frameVerdict({ ...onset, ...changed }, 'onset'), false);
  }
  const release = { fps: 29.9, frames: 27, meanMs: 21.6, maxMs: 38.6, late: 3, gapMs: 52.3 };
  assert.equal(frameVerdict(release, 'release'), true);
  assert.equal(frameVerdict({ ...release, frames: 20, late: 4 }, 'release'), true);
  assert.equal(frameVerdict({ ...release, frames: 19 }, 'release'), false);
  assert.equal(frameVerdict({ ...release, late: 5 }, 'release'), false);
  const stable = { fps: 30, frames: 139, meanMs: 21.960, maxMs: 31.628, late: 0, gapMs: 50.779 };
  assert.equal(frameVerdict(stable), true);
  assert.equal(frameVerdict({ ...stable, late: 1 }), false);
  assert.equal(frameVerdict({ ...stable, fps: 29.4 }), false);
  assert.equal(frameVerdict({ ...stable, gapMs: NaN }), false);
  const reaction = { fps: 30, frames: 110, meanMs: 20.8, maxMs: 33.9, late: 1, gapMs: 53 };
  assert.equal(frameVerdict(reaction, 'reaction'), true);
  assert.equal(frameVerdict({ ...reaction, late: 3 }, 'reaction'), false);
  assert.equal(frameVerdict({ ...reaction, fps: 29.4 }, 'reaction'), false);
  assert.equal(frameVerdict({ ...reaction, frames: 60, late: 5 }, 'conversation'), true);
  assert.equal(frameVerdict({ ...reaction, frames: 59 }, 'conversation'), false);
  assert.equal(frameVerdict({ ...reaction, late: 6 }, 'conversation'), false);
  assert.equal(frameVerdict(reaction, 'unknown'), false);
});
test('cadence is parsed from actual panel first-write spacing, and missing diagnostics fail', () => {
  const stats = parseStats('display: 29.1 fps (26 frames), 26759 us/frame (max 38852, 5 over 33.3 ms)\n' +
    'present: frames shown 16818..52070 us apart, character Plush');
  assert.equal(stats.gapMs, 52.070);
  assert.equal(frameVerdict(stats, 'onset'), true);
  assert.equal(frameVerdict(parseStats('no statistics'), 'onset'), false);
});
