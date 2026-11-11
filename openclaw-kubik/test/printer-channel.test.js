import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate as yieldIo } from 'node:timers/promises';
import { PrinterChannel } from '../src/printer-session.js';
import { printerResult } from './printer-fixture.js';

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
        await yieldIo();assert.equal(sent.length, 1);assert.deepEqual(sent[0], request);assert.equal(settled, false);
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
