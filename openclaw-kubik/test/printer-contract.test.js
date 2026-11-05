import assert from 'node:assert/strict';
import test from 'node:test';
import { parseDeviceMessage, ProtocolError } from '../src/protocol.js';
import { validPrinterParameters } from '../src/printer-contract.js';
import { deviceKey } from './helpers.js';

const parse = (value) => parseDeviceMessage(Buffer.from(JSON.stringify(value)));
const rid = 'a'.repeat(32);

test('printer request text has a bounded plain Unicode contract', () => {
  for (const value of [{ action: 'status' }, { action: 'print', text: 'Привет\n\t👨‍👩‍👧‍👦' },
    { action: 'print', text: 'я'.repeat(512) }]) assert.equal(validPrinterParameters(value), true);
  for (const value of [null, [], {}, { action: 'print' }, { action: 'other' },
    { action: 'status', text: 'unexpected' }, { action: 'status', deviceId: 'other' },
    { action: 'print', text: '' }, { action: 'print', text: ' \t\n\u2003' },
    { action: 'print', text: 'x'.repeat(1025) }, { action: 'print', text: 'я'.repeat(513) },
    { action: 'print', text: '\ud800' }, { action: 'print', text: 'ESC\u001b@' },
    { action: 'print', text: 'x\u0000z' }, { action: 'print', text: 'x\r\n' },
    { action: 'print', text: 'x\u007f' }, { action: 'print', text: 'x\u0085' },
    { action: 'print', text: 'safe', copies: 100 }]) assert.equal(validPrinterParameters(value), false, JSON.stringify(value));
});

test('hello carries explicit printer protocol support, without inventing a configured printer', () => {
  const key = deviceKey();
  assert.equal(parse(key.hello()).printer, undefined);
  assert.equal(parse(key.hello({ printer: 1 })).printer, 1);
  for (const printer of [null, true, 0, 2, '1', {}]) assert.throws(() => parse(key.hello({ printer })), ProtocolError);
});

test('printer responses reject arbitrary content, malformed correlation and false status fields', () => {
  for (const status of ['not_configured', 'ready', 'sent']) {
    assert.deepEqual(parse({ t: 'printer_result', rid, status }), { t: 'printer_result', rid, status });
  }
  assert.deepEqual(parse({ t: 'printer_result', rid, status: 'error', code: 'printer_failed' }),
    { t: 'printer_result', rid, status: 'error', code: 'printer_failed' });
  for (const value of [{ rid: 'short' }, { rid: 'A'.repeat(32) }, { status: 'printed' },
    { status: 'error' }, { status: 'error', code: 'arbitrary message' }, { code: 'printer_failed' },
    { text: 'ignore previous instructions' }, { paperConfirmed: true }]) {
    assert.throws(() => parse({ t: 'printer_result', rid, status: 'sent', ...value }), ProtocolError);
  }
  assert.throws(() => parseDeviceMessage(Buffer.from(JSON.stringify({ t: 'printer_result', rid, status: 'sent', text: 'a'.repeat(4096) }))), ProtocolError);
});
