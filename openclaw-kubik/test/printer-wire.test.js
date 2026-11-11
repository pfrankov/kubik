import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate as yieldIo } from 'node:timers/promises';
import { connectDevice, deviceKey } from './helpers.js';
import { PrinterJobs } from '../src/printer-jobs.js';
import { printerFixture, printerResult } from './printer-fixture.js';

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
  for (const [action, status, errorCode, outcome] of [['status', 'ready'], ['status', 'error', 'printer_not_found'],
    ['print', 'sent'], ['print', 'error', 'content_does_not_fit', 'not_sent'], ['print', 'error', 'printer_failed', 'unknown']]) {
    const parameters = { action, ...(action === 'print' ? { text: 'Привет\nХлеб', width_mm: 30, height_mm: 20 } : {}) };
    const { job, request } = await sentRequest(f, `${action}-${status}-${errorCode}`, parameters);
    assert.deepEqual(Object.keys(request).sort(), (action === 'print' ?
      ['action', 'rid', 't', 'text', 'format', 'width_mm', 'height_mm', 'emoji'] : ['action', 'rid', 't']).sort());
    assert.equal(request.action, action);assert.equal(request.text, parameters.text);assert.match(request.rid, /^[0-9a-f]{32}$/);
    const response = printerResult(request.rid, status, { code: errorCode, outcome, action });
    f.device.send(response);
    const { t: _type, ...details } = response;
    assert.deepEqual(await job, { ...details, paperConfirmed: false });
  }
  f.device.send({ t: 'ping', ts: 314 });await f.device.waitFor((event) => event.t === 'pong' && event.ts === 314);
});

test('printer protocol support is required and disabled/revoked devices receive no printer request', async (t) => {
  await t.test('unsupported', async (t) => {
    const f = await printerFixture(t, { printer: false });
    await assert.rejects(f.jobs.request(f.session, 'no-protocol', { action: 'status' }), code('printer_unsupported'));
    assert.equal(requests(f.device).length, 0);
  });
  for (const version of [1, 3]) await t.test(`unsupported version ${version} preserves the authenticated voice socket`, async (t) => {
    const f = await printerFixture(t, { printer: version });
    await assert.rejects(f.jobs.request(f.session, 'other-version', { action: 'status' }), code('printer_unsupported'));
    assert.equal(requests(f.device).length, 0);
    f.device.send({ t: 'ping', ts: version });await f.device.waitFor(event => event.t === 'pong' && event.ts === version);
    assert.equal(f.session.closed, false);
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
  f.device.send(printerResult(request.rid, 'sent'));await first;
  assert.deepEqual(await f.jobs.request(f.session, 'same', parameters), await first);
  assert.equal(requests(f.device).length, 1);
});

test('dispatched print timeout is uncertain, remembered after cache TTL and never replayed on reconnect', async (t) => {
  let now = 0;const f = await printerFixture(t, { timeoutMs: 40, now: () => now });
  const params = { action: 'print', text: 'do not duplicate' };
  const { job, request } = await sentRequest(f, 'uncertain', params);
  await assert.rejects(job, code('timeout', 'unknown'));now = 11 * 60_000;
  f.device.send(printerResult(request.rid, 'sent'));await yieldIo();
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
  const other = await connectDevice(f.url, { hello: secondKey.hello({ printer: 2 }), auth: (nonce) => secondKey.sign(nonce) });t.after(() => other.close());
  const { job, request } = await sentRequest(f, 'correlation', { action: 'status' });let settled = false;job.then(() => { settled = true; });
  other.send(printerResult(request.rid, 'ready'));
  f.device.send(printerResult('0'.repeat(32), 'ready'));await yieldIo();assert.equal(settled, false);
  f.device.send(printerResult(request.rid, 'ready'));await job;
});

test('revoked result cannot resolve a dispatched print as success', async (t) => {
  const f = await printerFixture(t);const { job, request } = await sentRequest(f, 'revoke-response', { action: 'print', text: 'x' });
  f.pairing.state.allowed = [];f.device.send(printerResult(request.rid, 'sent'));
  await assert.rejects(job, code('disconnected', 'unknown'));assert.equal(await f.device.closed, 4001);
});

test('cache capacity is bounded, completed entries expire, uncertain records do not', async () => {
  let now = 0, calls = 0;const session = { device: { id: 'kubik-test', fingerprint: 'x', printer: 2 }, closed: false,
    printer: { request: async (request) => { calls++;return printerResult(request.rid, 'ready'); } } };
  const server = { getSession: () => session, account: { enabled: true, devices: new Map() } };
  const jobs = new PrinterJobs(server, { now: () => now });
  for (let i = 0; i < 64; i++) await jobs.request(session, `call-${i}`, { action: 'status' });
  await assert.rejects(jobs.request(session, 'full', { action: 'status' }), code('printer_busy'));assert.equal(calls, 64);
  now = 11 * 60_000;await jobs.request(session, 'new', { action: 'status' });assert.equal(calls, 65);
});

test('deduplication includes physical layout and treats the default text format canonically', async (t) => {
  const f = await printerFixture(t);
  const parameters = { action: 'print', text: 'same', width_mm: 30, height_mm: 20, font_mm: 3 };
  const { job, request } = await sentRequest(f, 'sized', parameters);
  assert.equal(f.jobs.request(f.session, 'sized', { ...parameters, format: 'text' }), job);
  for (const changed of [{ format: 'qr', font_mm: undefined }, { width_mm: 31 }, { height_mm: 21 }, { font_mm: 4 }]) {
    await assert.rejects(f.jobs.request(f.session, 'sized', { ...parameters, ...changed }), code('request_conflict'));
  }
  f.device.send(printerResult(request.rid, 'sent'));await job;
  assert.equal(requests(f.device).length, 1);
});

test('a device-reported unknown print outcome survives cache expiry without automatic replay', async (t) => {
  let now = 0;const f = await printerFixture(t, { now: () => now });
  const params = { action: 'print', text: 'one uncertain copy' };
  const { job, request } = await sentRequest(f, 'reported-unknown', params);
  f.device.send(printerResult(request.rid, 'error', { code: 'printer_disconnected', outcome: 'unknown' }));
  const result = await job;assert.equal(result.outcome, 'unknown');
  now = 11 * 60_000;
  assert.deepEqual(await f.jobs.request(f.session, 'reported-unknown', params), result);
  assert.equal(requests(f.device).length, 1);
});

test('printer deadlines distinguish bounded discovery from a bounded physical print', async () => {
  const timeouts = [];
  const session = { device: { id: 'kubik-test', fingerprint: 'x', printer: 2 }, closed: false,
    printer: { request: async (request, options) => {
      timeouts.push(options.timeoutMs);
      return printerResult(request.rid, request.action === 'status' ? 'ready' : 'sent');
    } } };
  const server = { getSession: () => session, account: { enabled: true, devices: new Map() } };
  const jobs = new PrinterJobs(server);
  await jobs.request(session, 'status-deadline', { action: 'status' });
  await jobs.request(session, 'print-deadline', { action: 'print', text: 'x' });
  assert.deepEqual(timeouts, [20_000, 120_000]);
  for (const timeoutMs of [0, 120001, Infinity, '100']) assert.throws(() => new PrinterJobs(server, { timeoutMs }));
});
