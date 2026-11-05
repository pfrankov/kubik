import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate as yieldIo } from 'node:timers/promises';
import { connectDevice, deviceKey } from './helpers.js';
import { printerFixture, toolFixture } from './printer-fixture.js';
import { printerJobs } from '../src/printer-jobs.js';
import { registerPrinterTool } from '../src/printer-tool.js';
import { PrinterError } from '../src/printer-contract.js';

test('printer tool is optional, model-visible only for the authoritative device/agent/account route', async (t) => {
  const f = await printerFixture(t);const { factory, context, tool } = toolFixture(f.server, f.identity.device);
  assert.equal(tool.name, 'kubik_printer');assert.equal(tool.parameters.additionalProperties, false);
  assert.deepEqual(Object.keys(tool.parameters.properties), ['action', 'text']);
  for (const changed of [{ messageChannel: 'telegram' }, { messageChannel: undefined }, { nativeChannelId: undefined },
    { nativeChannelId: 'kubik-other' }, { sessionKey: 'agent:main:main' }, { sessionKey: undefined },
    { agentId: 'other' }, { agentId: undefined }, { agentAccountId: 'foreign' }]) {
    assert.equal(factory({ ...context, ...changed }), null, JSON.stringify(changed));
  }
  assert.equal(factory({ ...context, nativeChannelId: '../escape' }), null);
  const registered = [];
  registerPrinterTool({ runtime: {}, config: {}, registerTool: (...args) => registered.push(args) });
  assert.equal(typeof registered[0][0], 'function');assert.deepEqual(registered[0][1], { name: 'kubik_printer', optional: true });
});

test('registered production factory sends a correlated operation and gives the model an honest not-configured result', async (t) => {
  const f = await printerFixture(t);const { tool } = toolFixture(f.server, f.identity.device);
  const pending = tool.execute('tool-print', { action: 'print', text: 'Print this list' });
  const request = await f.device.waitFor((event) => event.t === 'printer');
  f.device.send({ t: 'printer_result', rid: request.rid, status: 'not_configured' });
  const result = await pending;
  assert.equal(result.isError, true);assert.equal(result.details.code, 'printer_not_configured');
  assert.equal(result.details.status, 'not_configured');assert.equal(result.details.outcome, 'not_sent');
  assert.equal(result.details.paperConfirmed, false);assert.deepEqual(JSON.parse(result.content[0].text), result.details);
  assert.equal(JSON.stringify(result).includes('Print this list'), false, 'printed content is not echoed to tool logs/results');
  const status = tool.execute('tool-status', { action: 'status' });
  const statusRequest = await f.device.waitForNth((event) => event.t === 'printer', 2);
  f.device.send({ t: 'printer_result', rid: statusRequest.rid, status: 'not_configured' });
  assert.equal((await status).isError, undefined, 'reading an unavailable printer is still a successful status query');
});

test('tool rejects arguments before sending and uses current config, not its captured allow decision', async (t) => {
  const f = await printerFixture(t);const { tool, state } = toolFixture(f.server, f.identity.device);
  for (const params of [{ action: 'print', text: '\u001b@' }, { action: 'print', text: 'x', deviceId: 'kubik-other' },
    { action: 'print', text: '\ud800' }, { action: 'status', text: 'ignored?' }]) {
    assert.equal((await tool.execute('invalid', params)).details.code, 'invalid_input');
  }
  state.cfg = { ...state.cfg, channels: { kubik: { ...state.cfg.channels.kubik, enabled: false } } };
  assert.equal((await tool.execute('disabled-after-create', { action: 'print', text: 'x' })).details.code, 'device_replaced');
  assert.equal(f.device.events.some((event) => event.t === 'printer'), false);
});

test('a tool bound to an old socket/key cannot target its replacement or another server instance', async (t) => {
  const f = await printerFixture(t);const { tool } = toolFixture(f.server, f.identity.device);
  const otherKey = deviceKey(f.identity.device);f.pairing.state.allowed.push(otherKey.entry);
  const next = await connectDevice(f.url, { hello: otherKey.hello({ printer: 1 }), auth: (nonce) => otherKey.sign(nonce) });t.after(() => next.close());
  const result = await tool.execute('old-context', { action: 'print', text: 'x' });
  assert.equal(result.details.code, 'device_replaced');assert.equal(next.events.some((event) => event.t === 'printer'), false);
  let current = f.server;
  const fresh = toolFixture(f.server, f.identity.device, { serverFor: () => current });current = {};
  assert.equal((await fresh.tool.execute('old-server', { action: 'status' })).details.code, 'device_replaced');
});

test('configuration changes during the awaited pairing check cannot dispatch a printer request', async (t) => {
  const f = await printerFixture(t);const { tool, state } = toolFixture(f.server, f.identity.device);
  let release, entered;const checking = new Promise((resolve) => { entered = resolve; });
  f.pairing.api.readAllowFromStore = () => { entered();return new Promise((resolve) => { release = resolve; }); };
  const operation = tool.execute('configuration-race', { action: 'print', text: 'x' });await checking;
  state.cfg.channels.kubik.enabled = false;release([f.identity.entry]);
  assert.equal((await operation).details.code, 'device_replaced');assert.equal(f.device.events.some((event) => event.t === 'printer'), false);
});

test('module copies share the account job cache and do not resend an earlier effect', async (t) => {
  const f = await printerFixture(t);const original = printerJobs(f.server);
  const copy = await import('../src/printer-jobs.js?printer-module-copy');
  assert.equal(copy.printerJobs(f.server), original);
  const params = { action: 'print', text: 'one' };const job = original.request(f.session, 'module-copy', params);
  const request = await f.device.waitFor((event) => event.t === 'printer');
  f.device.send({ t: 'printer_result', rid: request.rid, status: 'sent' });await job;
  assert.equal((await copy.printerJobs(f.server).request(f.session, 'module-copy', params)).status, 'sent');
  await yieldIo();assert.equal(f.device.events.filter((event) => event.t === 'printer').length, 1);
});

test('foreign module errors retain their bounded meaning; arbitrary thrown values never leak fields', async (t) => {
  const f = await printerFixture(t);const { tool } = toolFixture(f.server, f.identity.device);
  const foreign = await import('../src/printer-contract.js?foreign-error-class');
  let failure = new foreign.PrinterError('timeout', { rid: 'a'.repeat(32), dispatched: true, action: 'print' });
  assert.equal(failure instanceof PrinterError, false, 'the fixture must use a distinct constructor');
  Object.defineProperty(f.server, Symbol.for('openclaw.kubik.printer-jobs'), { value: { request: async () => { throw failure; } } });
  const result = await tool.execute('foreign-error', { action: 'print', text: 'x' });
  assert.deepEqual(result.details, { status: 'error', code: 'timeout', rid: 'a'.repeat(32), outcome: 'unknown', paperConfirmed: false });
  for (const value of [null, undefined, 'private-provider-message', new Error('private-provider-message'),
    { code: 'timeout', rid: 'private-provider-message', outcome: 'not_sent' },
    { [Symbol.for('openclaw.kubik.printer-error')]: true, code: 'unbounded external code', dispatched: true, outcome: 'unknown' }]) {
    failure = value;
    const rejected = await tool.execute('unsafe-error', { action: 'print', text: 'x' });
    assert.deepEqual(rejected.details, { status: 'error', code: 'printer_failed', outcome: 'unknown', paperConfirmed: false });
    assert.equal(JSON.stringify(rejected).includes('private-provider-message'), false);
  }
});
