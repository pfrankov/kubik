import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate as yieldIo } from 'node:timers/promises';
import { PrinterChannel } from '../src/printer-session.js';

test('early matching printer replies cannot complete work before authorization and dispatch', async (t) => {
  for (const authorization of [true, false, 'throw']) {
    await t.test(String(authorization), async () => {
      let release, reject, settled = false;const sent = [];
      const permit = new Promise((yes, no) => { release = yes;reject = no; });
      const session = { closed: false, ws: { readyState: 1 }, current: () => session,
        authorize: () => permit, send: (message) => { sent.push(message);return true; } };
      const channel = new PrinterChannel(session);
      const request = { t: 'printer', rid: 'b'.repeat(32), action: 'print', text: 'one copy' };
      const reply = { t: 'printer_result', rid: request.rid, status: 'sent' };
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
