import assert from 'node:assert/strict';
import { rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import test from 'node:test';
import { preparePrinterRequest } from '../src/printer-emoji.js';
import { printerFixture, printerLayout, printerResult, toolFixture } from './printer-fixture.js';
import { syntheticAtlas } from './printer-emoji-fixture.js';

const code = expected => error => error.code === expected && error.outcome === 'not_sent';
const requests = device => device.events.filter(event => event.t === 'printer');
const atlasPath = fixture => join(fixture.server.stateDir, 'apple-emoji.bin');

test('authenticated broker enriches text once from its private atlas, deduping before artwork access', async t => {
  const f = await printerFixture(t); writeFileSync(atlasPath(f), syntheticAtlas());
  let loads = 0; const load = f.jobs.emojiAtlas.load.bind(f.jobs.emojiAtlas);
  f.jobs.emojiAtlas.load = () => { loads++; return load(); };
  const params = { action: 'print', text: 'Milk 😀 😀 🇷🇺 ❤️' };
  const job = f.jobs.request(f.session, 'emoji-copy', params);
  assert.equal(f.jobs.request(f.session, 'emoji-copy', params), job);
  const wire = await f.device.waitFor(event => event.t === 'printer');
  assert.equal(wire.text, params.text); assert.equal(wire.format, 'text');
  assert.deepEqual(wire.emoji.map(item => item.sequence), ['😀', '🇷🇺', '❤️']);
  assert.equal(loads, 1); assert.ok(Buffer.byteLength(JSON.stringify(wire)) <= 4096);
  f.device.send(printerResult(wire.rid, 'sent')); const result = await job;
  assert.equal(JSON.stringify(result).includes('bitmap'), false);
  rmSync(atlasPath(f)); assert.deepEqual(await f.jobs.request(f.session, 'emoji-copy', params), result);
  assert.equal(loads, 1); assert.equal(requests(f.device).length, 1);
  await assert.rejects(f.jobs.request(f.session, 'new-emoji-copy', params), code('unsupported_character'));
  assert.equal(requests(f.device).length, 1);
});

test('eight used glyphs fit a real RPC, a ninth fails before wire without discarding dedupe', async t => {
  const f = await printerFixture(t), sequences = Array.from({ length: 9 }, (_, i) => String.fromCodePoint(0x1f600 + i));
  writeFileSync(atlasPath(f), syntheticAtlas(sequences));
  const first = f.jobs.request(f.session, 'eight', { action: 'print', text: sequences.slice(0, 8).join('') });
  const wire = await f.device.waitFor(event => event.t === 'printer'); assert.equal(wire.emoji.length, 8);
  f.device.send(printerResult(wire.rid, 'sent')); await first;
  const params = { action: 'print', text: sequences.join('') };
  await assert.rejects(f.jobs.request(f.session, 'nine', params), code('invalid_request'));
  await assert.rejects(f.jobs.request(f.session, 'nine', params), code('invalid_request'));
  assert.equal(requests(f.device).length, 1);
});

test('missing private artwork leaves ordinary text and QR usable while emoji fails explicitly', async t => {
  const f = await printerFixture(t), { tool } = toolFixture(f.server, f.identity.device);
  const unsupported = await tool.execute('missing-art', { action: 'print', text: '😀' });
  assert.equal(unsupported.isError, true); assert.equal(unsupported.details.code, 'unsupported_character');
  assert.equal(unsupported.details.outcome, 'not_sent'); assert.equal(requests(f.device).length, 0);
  for (const format of ['text', 'qr']) {
    const parameters = { action: 'print', format, text: format === 'text' ? 'Привет Bread' : '😀' };
    const job = f.jobs.request(f.session, format, parameters);
    const wire = await f.device.waitForNth(event => event.t === 'printer', requests(f.device).length + 1);
    if (format === 'text') assert.deepEqual(wire.emoji, []); else assert.equal(Object.hasOwn(wire, 'emoji'), false);
    const layout = { ...printerLayout(), format }; if (format === 'qr') delete layout.font_mm;
    f.device.send(printerResult(wire.rid, 'sent', { layout })); await job;
  }
});

test('fresh authorization runs before opening the private atlas', async t => {
  const f = await printerFixture(t); let loads = 0;
  f.jobs.emojiAtlas.load = () => { loads++; throw Error('must not read'); };
  f.session.authorize = async () => false;
  await assert.rejects(f.jobs.request(f.session, 'revoked-art', { action: 'print', text: '😀' }), code('unauthorized'));
  assert.equal(loads, 0); assert.equal(requests(f.device).length, 0);
});

test('the final RPC boundary measures exact UTF-8 JSON bytes, accepting 4096 and rejecting 4097', async t => {
  const f = await printerFixture(t);
  const base = { t: 'printer', rid: 'b'.repeat(32), action: 'print', format: 'text', text: '' };
  const overhead = Buffer.byteLength(JSON.stringify(preparePrinterRequest(base)));
  // Exercise the independent transport bound directly; public tool text already has a stricter 1024-byte limit.
  const sized = size => {
    const available = size - overhead;
    return { ...base, text: 'я'.repeat(Math.floor(available / 2)) + 'x'.repeat(available % 2) };
  };
  const exact = f.session.printer.request(sized(4096), { allowed: () => true, timeoutMs: 500 });
  const wire = await f.device.waitFor(event => event.t === 'printer');
  assert.equal(Buffer.byteLength(JSON.stringify(wire)), 4096);
  assert.ok(JSON.stringify(wire).includes('я')); assert.equal(JSON.stringify(wire).includes('\\u044f'), false);
  f.device.send(printerResult(wire.rid, 'error', { code: 'invalid_request' })); await exact;
  await assert.rejects(f.session.printer.request(sized(4097), { allowed: () => true, timeoutMs: 500 }), code('invalid_request'));
  assert.equal(requests(f.device).length, 1);
});
