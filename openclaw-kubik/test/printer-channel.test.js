import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate as yieldIo } from 'node:timers/promises';
import { PrinterChannel } from '../src/printer-session.js';
import { preparePrinterRequest } from '../src/printer-emoji.js';
import { printerLayout, printerResult } from './printer-fixture.js';

test('early matching printer replies cannot complete work before authorization and dispatch', async (t) => {
  for (const authorization of [true, false, 'throw']) {
    await t.test(String(authorization), async () => {
      let release, reject, settled = false;const sent = [];
      const permit = new Promise((yes, no) => { release = yes;reject = no; });
      const session = { closed: false, ws: { readyState: 1 }, current: () => session,
        acquireOutputLease: async () => () => {}, authorize: () => permit, send: (message) => { sent.push(message);return true; } };
      const channel = new PrinterChannel(session);
      const request = { t: 'printer', rid: 'b'.repeat(32), action: 'print', text: 'one copy' };
      const reply = printerResult(request.rid, 'sent');
      const result = channel.request(request, { allowed: () => true, timeoutMs: 500 });
      result.then(() => { settled = true; }, () => { settled = true; });
      assert.equal(channel.accept(reply), false, 'an early matching reply is not proof that work was sent');
      await yieldIo();assert.equal(settled, false);assert.equal(sent.length, 0);
      if (authorization === 'throw') reject(new Error('private store error'));
      else release(authorization);
      if (authorization === true) {
        await yieldIo();assert.equal(sent.length, 1);assert.deepEqual(sent[0], { ...request, emoji: [] });assert.equal(settled, false);
        assert.equal(channel.accept(reply), true);
        assert.equal((await result).status, 'sent');
      } else {
        await assert.rejects(result, (error) => error.code === 'unauthorized' && error.outcome === 'not_sent');
        assert.equal(sent.length, 0);assert.equal(channel.accept(reply), false);
      }
    });
  }
});

test('printer replies cannot silently exceed the requested dimensions or change format', async (t) => {
  for (const extra of [{ width_mm: 10 }, { height_mm: 5 }, { font_mm: 2 }, { format: 'qr' }]) {
    await t.test(JSON.stringify(extra), async () => {
      const session = { closed: false, ws: { readyState: 1 }, current: () => session,
        acquireOutputLease: async () => () => {}, authorize: async () => true, send: () => true };
      const channel = new PrinterChannel(session);
      const request = { t: 'printer', rid: 'c'.repeat(32), action: 'print', text: 'size', ...extra };
      const operation = channel.request(request, { allowed: () => true, timeoutMs: 500 });
      await yieldIo();
      const failure = assert.rejects(operation, error => error.code === 'invalid_response' && error.outcome === 'unknown');
      assert.equal(channel.accept(printerResult(request.rid, 'sent')), false);
      await failure;
    });
  }
});

test('status and print errors carry action-appropriate outcomes', async (t) => {
  for (const [action, hasOutcome, accepted] of [['status', false, true], ['status', true, false],
    ['print', false, false], ['print', true, true]]) {
    await t.test(action + String(hasOutcome), async () => {
      const session = { closed: false, ws: { readyState: 1 }, current: () => session,
        acquireOutputLease: async () => () => {}, authorize: async () => true, send: () => true };
      const channel = new PrinterChannel(session);
      const request = { t: 'printer', rid: 'd'.repeat(32), action, ...(action === 'print' ? { text: 'x' } : {}) };
      const operation = channel.request(request, { allowed: () => true, timeoutMs: 500 });
      await yieldIo();
      const response = { t: 'printer_result', rid: request.rid, status: 'error', code: 'printer_not_found',
        ...(hasOutcome ? { outcome: 'not_sent' } : {}) };
      if (accepted) {
        assert.equal(channel.accept(response), true);
        assert.equal((await operation).code, 'printer_not_found');
      } else {
        const failure = assert.rejects(operation, error => error.code === 'invalid_response');
        assert.equal(channel.accept(response), false);await failure;
      }
    });
  }
});

test('layout request tolerances use a head dot for width and a feed row for height and font', async t => {
  const actual = printerLayout();
  for (const [key, delta, accepted] of [['width_mm', .1, false], ['height_mm', .1, true],
    ['height_mm', .18, false], ['font_mm', .1, true], ['font_mm', .18, false]]) {
    await t.test(`${key}:${delta}`, async t => {
      const session = { closed: false, ws: { readyState: 1 }, current: () => session,
        acquireOutputLease: async () => () => {}, authorize: async () => true, send: () => true };
      const channel = new PrinterChannel(session); t.after(() => channel.close());
      const request = { t: 'printer', rid: 'e'.repeat(32), action: 'print', text: 'size', [key]: actual[key] - delta };
      const operation = channel.request(request, { allowed: () => true, timeoutMs: 500 });
      const failure = accepted ? null : assert.rejects(operation, error => error.code === 'invalid_response');
      await yieldIo(); assert.equal(channel.accept(printerResult(request.rid, 'sent')), accepted);
      if (accepted) assert.equal((await operation).status, 'sent'); else await failure;
    });
  }
});

test('cancellation or expiry during internal preparation cannot dispatch late work', async t => {
  for (const cancelled of [true, false]) await t.test(String(cancelled), async t => {
    let sent = 0, prepared = 0;
    const controller = new AbortController();
    const session = { closed: false, ws: { readyState: 1 }, current: () => session,
      acquireOutputLease: async () => () => {}, authorize: async () => true, send: () => { sent++; return true; } };
    const channel = new PrinterChannel(session); t.after(() => channel.close());
    const request = { t: 'printer', rid: 'f'.repeat(32), action: 'print', text: 'x' };
    const operation = channel.request(request, { signal: controller.signal, allowed: () => true, timeoutMs: 10,
      prepare(value) {
        prepared++;
        if (cancelled) controller.abort();
        else { const until = Date.now() + 15; while (Date.now() < until) { /* bounded synchronous file preparation */ } }
        return preparePrinterRequest(value);
      } });
    await assert.rejects(operation, error => error.code === (cancelled ? 'cancelled' : 'timeout') && error.outcome === 'not_sent');
    assert.equal(prepared, 1); assert.equal(sent, 0);
  });
});
