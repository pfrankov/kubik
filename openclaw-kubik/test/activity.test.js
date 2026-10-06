import assert from 'node:assert/strict';
import test from 'node:test';
import { ActivityTracker, toolActivity } from '../src/activity.js';

test('tool names map to the status-reaction categories', () => {
  assert.equal(toolActivity('web_search'), 'web');
  assert.equal(toolActivity('browser'), 'web');
  assert.equal(toolActivity('exec'), 'coding');
  assert.equal(toolActivity('deploy_app'), 'deploy');
  assert.equal(toolActivity('cargo_build'), 'build');
  assert.equal(toolActivity('playwright_click'), 'concierge');
  assert.equal(toolActivity('ha_call_service'), 'tool');
  assert.equal(toolActivity(''), 'tool');
});

test('tracker: own vs other runs, tool phases, compaction, end, stall and forgetting', async () => {
  let now = 1000, changes = 0;
  const tracker = new ActivityTracker({ onChange: () => changes++, debounceMs: 5, now: () => now });
  const mine = 'agent:main:kubik:direct:kubik-b6c634';
  assert.deepEqual(tracker.summary(mine), { own: '', other: '' });
  tracker.event({ runId: 'r0', stream: 'tool', data: { phase: 'start', name: 'exec' } });  // started before we listened
  assert.equal(tracker.size, 0);
  tracker.event({ runId: 'cron1', stream: 'lifecycle', sessionKey: 'agent:main:cron:x', data: { phase: 'start' } });
  assert.deepEqual(tracker.summary(mine), { own: '', other: 'thinking' });
  // Hidden runs carry the session key on lifecycle events only.
  tracker.event({ runId: 'r1', stream: 'lifecycle', sessionKey: mine, data: { phase: 'start' } });
  tracker.event({ runId: 'r1', stream: 'tool', data: { phase: 'start', name: 'web_search' } });
  assert.deepEqual(tracker.summary(mine), { own: 'web', other: 'thinking' });
  tracker.event({ runId: 'r1', stream: 'tool', data: { phase: 'update', name: 'web_search' } });
  assert.equal(tracker.summary(mine).own, 'web');
  tracker.event({ runId: 'r1', stream: 'tool', data: { phase: 'result', name: 'web_search' } });
  assert.equal(tracker.summary(mine).own, 'thinking');
  tracker.event({ runId: 'r1', stream: 'compaction', data: { phase: 'start' } });
  tracker.event({ runId: 'r1', stream: 'assistant', data: { delta: 'x' } });
  assert.equal(tracker.summary(mine).own, 'compacting');
  tracker.event({ runId: 'r1', stream: 'compaction', data: { phase: 'end' } });
  assert.equal(tracker.summary(mine).own, 'thinking');
  tracker.event({ runId: 'r1', stream: 'lifecycle', data: { phase: 'end' } });
  assert.deepEqual(tracker.summary(mine), { own: '', other: 'thinking' });
  assert.deepEqual(tracker.summary(undefined), { own: '', other: 'thinking' }, 'no session key: everything is "other"');
  await new Promise((r) => setTimeout(r, 20));
  assert.ok(changes >= 1 && changes < 8, 'changes are debounced');
  now += 31_000;
  assert.equal(tracker.summary(mine).other, 'stall', 'no progress for 30 s');
  tracker.event({ runId: 'cron1', stream: 'tool', data: { phase: 'update', name: 'exec' } });
  assert.equal(tracker.summary(mine).other, 'thinking', 'progress clears the stall');
  tracker.event({ runId: 'cron1', stream: 'lifecycle', data: { phase: 'error' } });
  assert.equal(tracker.size, 0);
  tracker.close();
});
