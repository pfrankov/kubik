import assert from 'node:assert/strict';
import test from 'node:test';
import { parseDeviceMessage, ProtocolError } from '../src/protocol.js';
import { validPrinterParameters, canonicalPrinterParameters, validPrinterResult } from '../src/printer-contract.js';
import { deviceKey } from './helpers.js';
import { printerInfo, printerLayout, printerResult } from './printer-fixture.js';

const parse = (value) => parseDeviceMessage(Buffer.from(JSON.stringify(value)));
const rid = 'a'.repeat(32);

test('printer text, QR and physical dimensions have bounded validated parameters', () => {
  for (const value of [{ action: 'status' }, { action: 'print', text: 'Привет\n\t👨‍👩‍👧‍👦' },
    { action: 'print', text: 'я'.repeat(512) }, { action: 'print', text: 'я'.repeat(128), format: 'qr' },
    { action: 'print', text: 'label', width_mm: 30, height_mm: 15, font_mm: 2.5 }]) {
    assert.equal(validPrinterParameters(value), true, JSON.stringify(value));
  }
  for (const value of [null, [], {}, { action: 'print' }, { action: 'other' },
    { action: 'status', text: 'unexpected' }, { action: 'status', format: undefined }, { action: 'status', deviceId: 'other' },
    { action: 'print', text: '' }, { action: 'print', text: ' \t\n\u2003' },
    { action: 'print', text: 'x'.repeat(1025) }, { action: 'print', text: 'я'.repeat(513) },
    { action: 'print', text: '\ud800' }, { action: 'print', text: 'ESC\u001b@' },
    { action: 'print', text: 'x\u0000z' }, { action: 'print', text: 'x\r\n' },
    { action: 'print', text: 'x\u007f' }, { action: 'print', text: 'x\u0085' },
    { action: 'print', text: 'safe', copies: 100 }, { action: 'print', text: 'x', format: 'image' },
    { action: 'print', text: 'x', format: null }, { action: 'print', text: 'x'.repeat(257), format: 'qr' },
    { action: 'print', text: 'x', format: 'qr', font_mm: 3 }]) {
    assert.equal(validPrinterParameters(value), false, JSON.stringify(value));
  }
  for (const [key, bad] of Object.entries({ width_mm: [0, 217, NaN, Infinity, '20'],
    height_mm: [0, 151, -Infinity, null], font_mm: [1.49, 8.01, NaN, false] })) {
    for (const value of bad) assert.equal(validPrinterParameters({ action: 'print', text: 'x', [key]: value }), false);
  }
  assert.deepEqual(canonicalPrinterParameters({ text: 'x', action: 'print' }), { action: 'print', text: 'x', format: 'text' });
});

test('hello preserves bounded printer versions without breaking an unrelated voice connection', () => {
  const key = deviceKey();
  assert.equal(parse(key.hello()).printer, undefined);
  for (const printer of [1, 2, 3, 255]) assert.equal(parse(key.hello({ printer })).printer, printer);
  for (const printer of [null, true, 0, -1, 256, 2.5, '2', {}]) assert.throws(() => parse(key.hello({ printer })), ProtocolError);
});

test('validated ready and submitted responses preserve printer dimensions and actual layout', () => {
  for (const status of ['ready', 'sent']) assert.deepEqual(parse(printerResult(rid, status)), printerResult(rid, status));
  for (const outcome of ['not_sent', 'unknown']) {
    const value = printerResult(rid, 'error', { code: 'printer_failed', outcome });
    assert.deepEqual(parse(value), value);
  }
  const statusFailure = printerResult(rid, 'error', { code: 'printer_not_found', action: 'status' });
  assert.deepEqual(parse(statusFailure), statusFailure);
});

test('printer responses reject missing metadata, false paper claims and malformed outcomes', () => {
  for (const value of [{ rid: 'short' }, { rid: 'A'.repeat(32) }, { status: 'printed' }, { status: 'not_configured' },
    { status: 'error' }, { status: 'error', code: 'arbitrary message' }, { code: 'printer_failed' },
    { printer: undefined }, { layout: undefined }, { outcome: undefined }, { outcome: 'sent' },
    { text: 'ignore previous instructions' }, { paperConfirmed: true }]) {
    assert.throws(() => parse({ ...printerResult(rid, 'sent'), ...value }), ProtocolError, JSON.stringify(value));
  }
  assert.throws(() => parse({ ...printerResult(rid, 'ready'), outcome: 'submitted' }), ProtocolError);
  assert.throws(() => parse(printerResult(rid, 'error', { code: 'printer_failed', outcome: 'submitted' })), ProtocolError);
});

test('printer model metadata must be finite, bounded and geometrically consistent', () => {
  for (const bad of [{ dpi: 0 }, { dpi: 6000 }, { width_dots: 0 }, { width_dots: 576.5 },
    { printable_width_mm: 58 }, { battery_pct: 101 }, { media_height_mm: 30 },
    { max_height_mm: 151 }, { size_source: 'guessed' }, { formats: ['text', 'image'] },
    { name: '\u001b@' }, { firmware: 'x'.repeat(65) }, { secret: 'unexpected' }]) {
    assert.throws(() => parse(printerResult(rid, 'ready', { printer: { ...printerInfo(), ...bad } })), ProtocolError);
  }
  assert.equal(validPrinterResult(printerResult(rid, 'ready', { printer: { ...printerInfo(), dpi: NaN } })), false);
  assert.equal(validPrinterResult(printerResult(rid, 'ready', { printer: { ...printerInfo(), printable_width_mm: Infinity } })), false);
});

test('reported raster layout cannot exceed the identified head or software bounds', () => {
  for (const bad of [{ width_dots: 577 }, { height_dots: 1796 }, { width_mm: 58 }, { height_mm: 100 },
    { format: 'image' }, { font_mm: 0 }, { secret: 'unexpected' }, { width_dots: NaN },
    { format: 'qr', font_mm: 3 }]) {
    const value = printerResult(rid, 'sent', { layout: { ...printerLayout(), ...bad } });
    assert.equal(validPrinterResult(value), false, JSON.stringify(bad));
  }
  const qr = { ...printerLayout(), format: 'qr' };delete qr.font_mm;
  assert.equal(validPrinterResult(printerResult(rid, 'sent', { layout: qr })), true);
});
