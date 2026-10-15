import test from 'node:test';
import assert from 'node:assert/strict';
import { createActivityRelay, MAX_ACTIVITY_DEVICES } from '../src/agent-sdk/events.js';

for (const expiredCount of [1, MAX_ACTIVITY_DEVICES - 1]) {
  test(`activity capacity reclaims ${expiredCount} expired snapshots before evicting live activity`, async (t) => {
    let now = 1000;
    let changes = 0;
    let views;
    const activity = createActivityRelay({ now: () => now, onChange: () => {
      changes++;
      views = ['kubik-live', 'kubik-new'].map(id => activity.summary(activity.sessionKeyFor(id)));
    } });
    t.after(() => activity.close());
    activity.set('kubik-live', { own: 'coding' }, 60_000);
    now = 2000;
    for (let i = 1; i < MAX_ACTIVITY_DEVICES; i++) {
      activity.set(`kubik-${i}`, { own: 'web' }, i <= expiredCount ? 1000 : 60_000);
    }
    assert.equal(activity.size, MAX_ACTIVITY_DEVICES);

    now = 3000;
    changes = 0;
    activity.set('kubik-new', { own: 'build' }, 60_000);
    const other = expiredCount === MAX_ACTIVITY_DEVICES - 1 ? 'coding' : 'web';
    assert.deepEqual(views, [{ own: 'coding', other: 'build' }, { own: 'build', other }]);
    assert.equal(activity.size, MAX_ACTIVITY_DEVICES - expiredCount + 1);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(changes, 1, 'the new snapshot must refresh all sessions without an extra expiry broadcast');
  });
}

test('activity capacity still evicts the oldest live snapshot when none have expired', (t) => {
  let now = 1000;
  const activity = createActivityRelay({ now: () => now });
  t.after(() => activity.close());
  for (let i = 0; i < MAX_ACTIVITY_DEVICES; i++) {
    activity.set(`kubik-${i}`, { own: 'web' }, 60_000);
    now++;
  }
  activity.set('kubik-new', { own: 'build' }, 60_000);
  assert.equal(activity.size, MAX_ACTIVITY_DEVICES);
  assert.equal(activity.summary(activity.sessionKeyFor('kubik-0')).own, '');
  assert.equal(activity.summary(activity.sessionKeyFor('kubik-1')).own, 'web');
  assert.equal(activity.summary(activity.sessionKeyFor('kubik-new')).own, 'build');
});

test('updating an existing device at capacity preserves every other live snapshot', (t) => {
  let now = 1000;
  const activity = createActivityRelay({ now: () => now });
  t.after(() => activity.close());
  for (let i = 0; i < MAX_ACTIVITY_DEVICES; i++) {
    activity.set(`kubik-${i}`, { own: 'web' }, 60_000);
    now++;
  }
  activity.set(' KUBIK:KUBIK-0 ', { own: 'coding' }, 60_000);
  assert.equal(activity.size, MAX_ACTIVITY_DEVICES);
  for (let i = 0; i < MAX_ACTIVITY_DEVICES; i++) {
    assert.equal(activity.summary(activity.sessionKeyFor(`kubik-${i}`)).own, i === 0 ? 'coding' : 'web');
  }
});
