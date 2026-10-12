import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import test from 'node:test';
import { getServer, monitorAccount } from '../src/monitor.js';

// Gateway retries a failed start. Each attempt must release its event subscription and sweep timer.
test('invalid persisted queue releases monitor resources and preserves the original file', async t => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-monitor-lifetime-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const invalid = '{not valid queue JSON';
  writeFileSync(join(dir, 'notifications.json'), invalid, { mode: 0o600 });
  const schedule = setInterval, cancel = clearInterval, active = new Set();
  t.mock.method(globalThis, 'setInterval', (...args) => {
    const timer = schedule(...args); active.add(timer); return timer;
  });
  t.mock.method(globalThis, 'clearInterval', timer => { active.delete(timer); return cancel(timer); });
  t.after(() => { for (const timer of active) cancel(timer); });
  for (const hasSignal of [true, false]) {
    let subscriptions = 0;
    const core = { state: { resolveStateDir: () => dir }, events: {
      onAgentEvent: () => { subscriptions++; return () => { subscriptions--; }; } } };
    const account = { enabled: true, accountId: 'lifetime-test', voice: {}, devices: new Map(), listen: { enabled: false } };
    for (let attempt = 0; attempt < 3; attempt++) {
      await assert.rejects(monitorAccount({ account, cfg: {},
        abortSignal: hasSignal ? new AbortController().signal : undefined }, {
        core, sdk: { channelStoppedPatch: value => value }, agentControl: {}, lan: { stateDir: dir },
      }), /Invalid notification queue JSON/);
      assert.equal(subscriptions, 0, 'failed startup retained an agent event subscription');
      assert.equal(active.size, 0, 'failed startup retained a sweep timer');
      assert.equal(getServer(account.accountId), undefined);
      assert.equal(readFileSync(join(dir, 'notifications.json'), 'utf8'), invalid);
    }
  }
});

test('normal account abort releases subscriptions, timers and the server registry', async t => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-monitor-abort-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const controller = new AbortController(), states = [], active = new Set();
  const schedule = setInterval, cancel = clearInterval;
  t.mock.method(globalThis, 'setInterval', (...args) => {
    const timer = schedule(...args); active.add(timer); return timer;
  });
  t.mock.method(globalThis, 'clearInterval', timer => { active.delete(timer); return cancel(timer); });
  t.after(() => { for (const timer of active) cancel(timer); });
  let subscriptions = 0, cronSubscriptions = 0;
  const account = { enabled: true, accountId: 'abort-test', voice: {}, devices: new Map(), listen: { enabled: false } };
  await monitorAccount({ account, cfg: {}, abortSignal: controller.signal,
    setStatus: status => { states.push(status.lifecycle); if (status.lifecycle === 'ready') controller.abort(); } }, {
    core: { state: { resolveStateDir: () => dir }, events: {
      onAgentEvent: () => { subscriptions++; return () => { subscriptions--; }; } } },
    sdk: { channelReadyPatch: value => ({ ...value, lifecycle: 'ready' }),
      channelStoppedPatch: value => ({ ...value, lifecycle: 'stopped' }) },
    agentControl: {}, lan: { stateDir: dir }, cron: {
      subscribe: () => { cronSubscriptions++; return () => { cronSubscriptions--; }; }, refresh() {},
    },
  });
  assert.deepEqual(states, ['starting', 'ready', 'stopped']);
  assert.equal(subscriptions, 0); assert.equal(cronSubscriptions, 0); assert.equal(active.size, 0);
  assert.equal(getServer(account.accountId), undefined);
});

test('cancelled startup stays stopped and a fresh start still becomes ready', async t => {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-monitor-startup-abort-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  for (const stopAt of ['before', 'pending', 'ready']) {
    const reachedReady = stopAt === 'ready';
    const controller = new AbortController(), states = [];
    let subscriptions = 0, cronSubscriptions = 0, cronRefreshes = 0;
    const account = { enabled: true, accountId: 'startup-abort-test', voice: {}, devices: new Map(), listen: { enabled: false } };
    if (stopAt === 'before') controller.abort();
    const running = monitorAccount({ account, cfg: {}, abortSignal: controller.signal,
      setStatus: status => {
        states.push(status);
        if (reachedReady && status.lifecycle === 'ready') controller.abort();
      } }, {
      core: { state: { resolveStateDir: () => dir }, events: {
        onAgentEvent: () => { subscriptions++; return () => { subscriptions--; }; } } },
      sdk: { channelReadyPatch: value => ({ ...value, lifecycle: 'ready' }),
        channelStoppedPatch: value => ({ ...value, lifecycle: 'stopped' }) },
      agentControl: {}, lan: { stateDir: dir }, cron: {
        subscribe: () => { cronSubscriptions++; return () => {}; },
        refresh: () => { cronRefreshes++; },
      },
    });
    // Even disabled LAN startup yields. Abort in that gap, without a clock or a real socket.
    if (!reachedReady) controller.abort();
    await running;
    assert.deepEqual(states.map(status => status.lifecycle), reachedReady
      ? ['starting', 'ready', 'stopped'] : ['starting', 'stopped']);
    assert.ok(states.every(status => !status.lastError), 'normal cancellation became a startup error');
    assert.equal(cronSubscriptions, Number(reachedReady), 'only a live startup subscribes to cron');
    assert.equal(cronRefreshes, Number(reachedReady), 'only a live startup starts cron work');
    assert.equal(subscriptions, 0, 'cancelled startup retained an activity subscription');
    assert.equal(getServer(account.accountId), undefined);
  }
});
