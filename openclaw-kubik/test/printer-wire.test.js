import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate as yieldIo } from 'node:timers/promises';
import { connectDevice, deviceKey } from './helpers.js';
import { PrinterJobs } from '../src/printer-jobs.js';
import { printerFixture } from './printer-fixture.js';

const code = (expected, outcome) => (error) => error.code === expected && (!outcome || error.outcome === outcome);
const requests = (device) => device.events.filter((event) => event.t === 'printer');

async function sentRequest(f, call, parameters) {
  const job = f.jobs.request(f.session, call, parameters);
  job.catch(() => {});
  const request = await f.device.waitForNth((event) => event.t === 'printer', requests(f.device).length + 1);
  return { job, request };
}

test('real authenticated socket carries only the requested printer job and reports sent rather than paper output', async (t) => {
  const f = await printerFixture(t);
  for (const [action, status, errorCode] of [['status', 'ready'], ['status', 'not_configured'],
    ['print', 'sent'], ['print', 'not_configured'], ['print', 'error', 'printer_failed']]) {
    const parameters = { action, ...(action === 'print' ? { text: 'Привет\n👨‍👩‍👧‍👦' } : {}) };
    const { job, request } = await sentRequest(f, `${action}-${status}`, parameters);
    assert.deepEqual(Object.keys(request).sort(), (action === 'print' ? ['action', 'rid', 't', 'text'] : ['action', 'rid', 't']).sort());
    assert.equal(request.action, action);assert.equal(request.text, parameters.text);assert.match(request.rid, /^[0-9a-f]{32}$/);
    f.device.send({ t: 'printer_result', rid: request.rid, status, ...(errorCode ? { code: errorCode } : {}) });
    const outcome = action !== 'print' ? undefined : status === 'sent' ? 'sent' : errorCode === 'printer_failed' ? 'unknown' : 'not_sent';
    assert.deepEqual(await job, { rid: request.rid, status, ...(errorCode ? { code: errorCode } : {}),
      ...(outcome ? { outcome } : {}), paperConfirmed: false });
  }
  f.device.send({ t: 'ping', ts: 314 });await f.device.waitFor((event) => event.t === 'pong' && event.ts === 314);
});

test('printer protocol support is required and disabled/revoked devices receive no printer request', async (t) => {
  await t.test('unsupported', async (t) => {
    const f = await printerFixture(t, { printer: false });
    await assert.rejects(f.jobs.request(f.session, 'no-protocol', { action: 'status' }), code('printer_unsupported'));
    assert.equal(requests(f.device).length, 0);
  });
  await t.test('disabled', async (t) => {
    const f = await printerFixture(t);f.server.account.devices.set(f.identity.device, { enabled: false });
    await assert.rejects(f.jobs.request(f.session, 'disabled', { action: 'print', text: 'safe' }), code('unauthorized'));
    assert.equal(requests(f.device).length, 0);
  });
  await t.test('revoked', async (t) => {
    const f = await printerFixture(t);f.pairing.state.allowed = [];
    await assert.rejects(f.jobs.request(f.session, 'revoked', { action: 'print', text: 'safe' }), code('disconnected', 'not_sent'));
    assert.equal(requests(f.device).length, 0);assert.equal(await f.device.closed, 4001);
  });
});

test('one in-flight job, same-call replay and conflicting reuse never duplicate printer work', async (t) => {
  const f = await printerFixture(t);
  const parameters = { action: 'print', text: 'one copy' };
  const first = f.jobs.request(f.session, 'same', parameters);
  const duplicate = f.jobs.request(f.session, 'same', { text: 'one copy', action: 'print' });
  assert.equal(first, duplicate);
  await assert.rejects(f.jobs.request(f.session, 'same', { action: 'print', text: 'different' }), code('request_conflict'));
  await assert.rejects(f.jobs.request(f.session, 'parallel', parameters), code('printer_busy'));
  const request = await f.device.waitFor((event) => event.t === 'printer');
  f.device.send({ t: 'printer_result', rid: request.rid, status: 'sent' });await first;
  assert.deepEqual(await f.jobs.request(f.session, 'same', parameters), await first);
  assert.equal(requests(f.device).length, 1);
});

test('dispatched print timeout is uncertain, remembered after cache TTL and never replayed on reconnect', async (t) => {
  let now = 0;const f = await printerFixture(t, { timeoutMs: 40, now: () => now });
  const params = { action: 'print', text: 'do not duplicate' };
  const { job, request } = await sentRequest(f, 'uncertain', params);
  await assert.rejects(job, code('timeout', 'unknown'));now = 11 * 60_000;
  f.device.send({ t: 'printer_result', rid: request.rid, status: 'sent' });await yieldIo();
  await assert.rejects(f.jobs.request(f.session, 'uncertain', params), code('timeout', 'unknown'));
  const next = await connectDevice(f.url, f.options);t.after(() => next.close());
  await assert.rejects(f.jobs.request(f.server.getSession(f.identity.device), 'uncertain', params), code('timeout', 'unknown'));
  assert.equal(requests(next).length, 0);assert.equal(requests(f.device).length, 1);
  next.send({ t: 'ping', ts: 99 });await next.waitFor((event) => event.t === 'pong' && event.ts === 99);
});

test('abort before send is safe and abort after send reports an unknown print outcome', async (t) => {
  const f = await printerFixture(t);const early = new AbortController();early.abort();
  await assert.rejects(f.jobs.request(f.session, 'early', { action: 'print', text: 'x' }, { signal: early.signal }), code('cancelled', 'not_sent'));
  const late = new AbortController();const job = f.jobs.request(f.session, 'late', { action: 'print', text: 'x' }, { signal: late.signal });job.catch(() => {});
  await f.device.waitFor((event) => event.t === 'printer');late.abort();await assert.rejects(job, code('cancelled', 'unknown'));
  assert.equal(requests(f.device).length, 1);
});

test('replacement while authorizing cannot send and replacement after dispatch cannot claim success', async (t) => {
  const f = await printerFixture(t);
  let release, entered;const waiting = new Promise((resolve) => { entered = resolve; });
  const read = f.pairing.api.readAllowFromStore;let hold = true;
  f.pairing.api.readAllowFromStore = (params) => {
    if (!hold) return read(params);hold = false;entered();return new Promise((resolve) => { release = resolve; });
  };
  const job = f.jobs.request(f.session, 'replace-before', { action: 'print', text: 'x' });job.catch(() => {});await waiting;
  const next = await connectDevice(f.url, f.options);t.after(() => next.close());
  release([f.identity.entry]);await assert.rejects(job, code('disconnected', 'not_sent'));assert.equal(requests(f.device).length, 0);
  const session = f.server.getSession(f.identity.device);const sent = f.jobs.request(session, 'replace-after', { action: 'print', text: 'x' });sent.catch(() => {});
  await next.waitFor((event) => event.t === 'printer');
  const newer = await connectDevice(f.url, f.options);t.after(() => newer.close());
  await assert.rejects(sent, code('disconnected', 'unknown'));assert.equal(requests(newer).length, 0);
});

test('unknown, stale and another device correlation cannot complete a request', async (t) => {
  const f = await printerFixture(t);const secondKey = deviceKey('kubik-other');f.pairing.state.allowed.push(secondKey.entry);
  const other = await connectDevice(f.url, { hello: secondKey.hello({ printer: 1 }), auth: (nonce) => secondKey.sign(nonce) });t.after(() => other.close());
  const { job, request } = await sentRequest(f, 'correlation', { action: 'status' });let settled = false;job.then(() => { settled = true; });
  other.send({ t: 'printer_result', rid: request.rid, status: 'ready' });
  f.device.send({ t: 'printer_result', rid: '0'.repeat(32), status: 'ready' });await yieldIo();assert.equal(settled, false);
  f.device.send({ t: 'printer_result', rid: request.rid, status: 'ready' });await job;
});

test('revoked result cannot resolve a dispatched print as success', async (t) => {
  const f = await printerFixture(t);const { job, request } = await sentRequest(f, 'revoke-response', { action: 'print', text: 'x' });
  f.pairing.state.allowed = [];f.device.send({ t: 'printer_result', rid: request.rid, status: 'sent' });
  await assert.rejects(job, code('disconnected', 'unknown'));assert.equal(await f.device.closed, 4001);
});

test('cache capacity is bounded, completed entries expire, uncertain records do not', async () => {
  let now = 0, calls = 0;const session = { device: { id: 'kubik-test', fingerprint: 'x', printer: 1 }, closed: false,
    printer: { request: async (request) => { calls++;return { rid: request.rid, status: 'ready' }; } } };
  const server = { getSession: () => session, account: { enabled: true, devices: new Map() } };
  const jobs = new PrinterJobs(server, { now: () => now });
  for (let i = 0; i < 64; i++) await jobs.request(session, `call-${i}`, { action: 'status' });
  await assert.rejects(jobs.request(session, 'full', { action: 'status' }), code('printer_busy'));assert.equal(calls, 64);
  now = 11 * 60_000;await jobs.request(session, 'new', { action: 'status' });assert.equal(calls, 65);
});
