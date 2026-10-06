import assert from 'node:assert/strict';
import test from 'node:test';
import { channelPlugin } from '../src/channel.js';
import { connectDevice, DEVICE, fakeEngine, sleep } from './helpers.js';
import { start } from './server-fixture.js';

test('activity: agent runs reach the device as own/other; heartbeat typing shows as its own; resent on reconnect', async (t) => {
  const listeners = new Set();
  const events = { onAgentEvent: (fn) => { listeners.add(fn); return () => listeners.delete(fn); } };
  const emit = (evt) => listeners.forEach((fn) => fn(evt));
  const { url } = await start(t, { runtime: { extraCore: { events } } });
  assert.equal(listeners.size, 1);
  const device = await connectDevice(url);
  const activity = () => device.events.filter((e) => e.t === 'activity').map((e) => `${e.own}/${e.other}`);
  const mine = `agent:main:kubik:direct:${DEVICE}`;
  emit({ runId: 'c', stream: 'lifecycle', sessionKey: 'agent:main:cron:nightly', data: { phase: 'start' } });
  await sleep(400);
  emit({ runId: 'm', stream: 'lifecycle', sessionKey: mine, data: { phase: 'start' } });
  await sleep(350); // let the debounced thinking update reach the device before the tool phase
  emit({ runId: 'm', stream: 'tool', data: { phase: 'start', name: 'web_fetch' } });
  await sleep(400);
  emit({ runId: 'm', stream: 'lifecycle', data: { phase: 'end' } });
  emit({ runId: 'c', stream: 'lifecycle', data: { phase: 'end' } });
  await sleep(400);
  assert.deepEqual(activity(), ['/thinking', 'thinking/thinking', 'web/thinking', '/']);
  // A heartbeat that reports to this Kubik: its run is elsewhere, but it is shown as Kubik's own work.
  await channelPlugin.heartbeat.sendTyping({ to: `kubik:${DEVICE}`, accountId: 'default' });
  emit({ runId: 'h', stream: 'lifecycle', sessionKey: 'agent:main:main', data: { phase: 'start' } });
  await sleep(400);
  assert.deepEqual(activity().slice(4), ['thinking/'], 'the heartbeat run itself adds nothing new');
  const again = await connectDevice(url);  // a reconnect gets the current picture right away
  await sleep(50);
  assert.deepEqual(again.events.filter((e) => e.t === 'activity'), [{ t: 'activity', own: 'thinking', other: '' }]);
  await channelPlugin.heartbeat.clearTyping({ to: `kubik:${DEVICE}`, accountId: 'default' });
  await sleep(20);
  assert.deepEqual(again.events.filter((e) => e.t === 'activity').at(-1), { t: 'activity', own: '', other: 'thinking' });
  again.ws.close();
});

test('a second copy of the plugin module (the Gateway loads it more than once) still reaches the devices', async (t) => {
  const { server, url } = await start(t);
  const device = await connectDevice(url);
  await sleep(50);
  const copy = await import(`../src/send.js?copy=${Date.now()}`);
  const monitorCopy = await import(`../src/monitor.js?copy=${Date.now()}`);
  assert.equal(monitorCopy.getServer('default'), server);
  const result = await copy.sendPayload(`kubik:${DEVICE}`, { text: 'Готово.' }, { accountId: 'default' });
  assert.ok(result.meta.spokenChars > 0);
  device.ws.close();
});

test('the removed engine/voice preference frame is rejected without changing configuration', async (t) => {
  const made = [];
  const { url } = await start(t, { engineFactory: (v) => { made.push(v.provider); return fakeEngine(); } });
  const device = await connectDevice(url);
  await sleep(50);
  assert.deepEqual(device.events.filter((e) => e.t === 'prefs'), [], 'the old preference menu is never offered');
  device.send({ t: 'prefs', engine: 'realtime', voice: 'shimmer' });
  assert.equal(await device.closed, 4002, 'removed protocol frames are rejected');
  assert.deepEqual(made, ['openai-http'], 'the voice engine is the same one');
});

test('cron indicator: running jobs and the next one-shot job reach the device; a reconnect gets them again', async (t) => {
  const { CronWatcher } = await import('../src/cron.js');
  let now = Date.now();
  const cron = new CronWatcher({ now: () => now });
  const jobs = [{ id: 'r', enabled: true, schedule: { kind: 'at' }, state: { nextRunAtMs: now + 600_000 } },
    { id: 'd', enabled: true, schedule: { kind: 'cron' }, state: { nextRunAtMs: now + 60_000 } },
    { id: 'x', enabled: false, schedule: { kind: 'at' }, state: { nextRunAtMs: now + 60_000 } }];
  const ctx = { getCron: () => ({ list: async () => jobs }) };
  const { url } = await start(t, { cron });
  const device = await connectDevice(url);
  await sleep(50);
  assert.deepEqual(device.events.filter((e) => e.t === 'cron'), [], 'nothing to show: nothing sent');
  const seen = () => device.events.filter((e) => e.t === 'cron').map((e) => `${e.running}/${e.next}`);
  cron.event({ action: 'scheduled', jobId: 'd' }, ctx);  // lists the jobs (debounced)
  await sleep(700);
  cron.event({ action: 'started', jobId: 'd' }, ctx);
  cron.event({ action: 'finished', jobId: 'd' }, ctx);
  now += 30_000;
  await sleep(700);
  assert.deepEqual(seen(), ['0/600', '1/600', '0/600'], 'the countdown runs on the device');
  const again = await connectDevice(url);
  await sleep(50);
  assert.deepEqual(again.events.filter((e) => e.t === 'cron'), [{ t: 'cron', running: 0, next: 570 }]);
  cron.event({ action: 'removed', jobId: 'r' }, ctx);
  jobs.shift();
  await sleep(700);
  assert.deepEqual(again.events.filter((e) => e.t === 'cron').at(-1), { t: 'cron', running: 0, next: -1 });
  device.ws.close();
  again.ws.close();
});
