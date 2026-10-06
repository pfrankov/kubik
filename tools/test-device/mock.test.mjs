import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import test from 'node:test';
import { startMock } from './mock.mjs';

function fixture({ pauseError = false, restoreError = false, restoreRoute = true, startup = true } = {}) {
  const child = new EventEmitter(); child.stdout = new EventEmitter();
  let route = 'wifi', killed = false;
  child.kill = () => { killed = true; queueMicrotask(() => child.emit('close')); return true; };
  const opts = { port: 18999, holdMs: 240000, startupMs: 10, connectMs: 10, restoreMs: 10, closeMs: 10, intervalMs: 1,
    spawnChild: () => { queueMicrotask(() => { if (startup) child.stdout.emit('data', 'parrot on'); }); return child; },
    device: async () => ({ via: route, wifi_connected: true }),
    sim: async (_, { ms }) => {
      if (ms === 1) { if (restoreError) throw Error('restore config refused'); if (restoreRoute) route = 'wifi'; }
      else { route = 'usb'; child.stdout.emit('data', 'device connected'); if (pauseError) throw Error('lost pause reply'); }
    } };
  return { opts, killed: () => killed, route: () => route };
}

test('mock restores primary route; startup failure after pause also restores it', async () => {
  const normal = fixture(); const mock = await startMock(normal.opts);
  assert.equal(normal.route(), 'usb'); await mock.stop();
  assert.equal(normal.route(), 'wifi'); assert.equal(normal.killed(), true);
  const lost = fixture({ pauseError: true });
  await assert.rejects(startMock(lost.opts), /lost pause reply/);
  assert.equal(lost.route(), 'wifi'); assert.equal(lost.killed(), true);
  const startup = fixture({ startup: false });
  await assert.rejects(startMock(startup.opts), /mock startup/);
  assert.equal(startup.killed(), true);
});

test('failed restore command or missing primary route fails and still closes mock', async () => {
  const refused = fixture({ restoreError: true }); const first = await startMock(refused.opts);
  await assert.rejects(first.stop(), /restore config refused/); assert.equal(refused.killed(), true);
  const absent = fixture({ restoreRoute: false }); const second = await startMock(absent.opts);
  await assert.rejects(second.stop(), /original route/); assert.equal(absent.killed(), true);
});

test('process error is not proof of process exit; failed kills cannot pass cleanup', async () => {
  const item = fixture();
  const create = item.opts.spawnChild;
  let kills = 0;
  item.opts.spawnChild = (...args) => {
    const child = create(...args);
    child.kill = () => { kills++; queueMicrotask(() => child.emit('error', Error('kill refused'))); return false; };
    return child;
  };
  const mock = await startMock(item.opts);
  await assert.rejects(mock.stop(), /forced mock exit/);
  assert.equal(kills, 2);
});
