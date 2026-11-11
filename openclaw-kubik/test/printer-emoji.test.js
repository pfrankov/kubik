import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { AppleEmojiAtlas, parseAppleEmojiAtlas, preparePrinterRequest } from '../src/printer-emoji.js';
import { GOLDEN_SEQUENCES, syntheticAtlas } from './printer-emoji-fixture.js';

const print = text => ({ t: 'printer', rid: 'a'.repeat(32), action: 'print', format: 'text', text });
const errorCode = code => error => error.code === code && error.outcome === 'not_sent';
function fixture(t, sequences) {
  const stateDir = mkdtempSync(join(tmpdir(), 'kubik-emoji-'));
  t.after(() => rmSync(stateDir, { recursive: true, force: true }));
  const path = join(stateDir, 'apple-emoji.bin');
  writeFileSync(path, syntheticAtlas(sequences));
  return { stateDir, path, atlas: new AppleEmojiAtlas(stateDir) };
}

test('KEMJ synthetic golden is byte-identical across hosts and contains no artwork', () => {
  const bytes = syntheticAtlas();
  assert.equal(bytes.length, 932);
  assert.equal(createHash('sha256').update(bytes).digest('hex'), 'cf060e21a44e0901f043500367ab139695f590821dd8f442167371cbe2e66b7b');
  const entries = parseAppleEmojiAtlas(bytes);
  assert.equal(entries.size, GOLDEN_SEQUENCES.length);
  for (const [i, bitmap] of [...entries.values()].entries()) assert.deepEqual(bitmap, Buffer.alloc(128, i + 1));
});

test('KEMJ rejects malformed headers, offsets, flags, controls, UTF-8 and unsorted keys', () => {
  const edits = [bytes => bytes.write('NOPE'), bytes => { for (let i = 0; i < 4; i++) bytes[i] |= 128; }, bytes => bytes.writeUInt32LE(2, 4),
    bytes => bytes.writeUInt32LE(16, 8), bytes => bytes.writeUInt32LE(0, 12),
    bytes => bytes.writeUInt32LE(8193, 12), bytes => bytes.writeUInt32LE(36, 16),
    bytes => bytes.writeUInt32LE(20, 20), bytes => bytes.writeUInt32LE(1, 24),
    bytes => bytes.writeUInt32LE(bytes.length + 1, 28), bytes => bytes.writeUInt16LE(1, 42),
    bytes => bytes.writeUInt16LE(65, 40), bytes => bytes.writeUInt32LE(0, 32),
    bytes => bytes.writeUInt32LE(bytes.length - 4, 36), bytes => bytes.writeUInt32LE(bytes.readUInt32LE(24) + 1, 36),
    bytes => { bytes[bytes.readUInt32LE(32)] = 0; }, bytes => { bytes[bytes.readUInt32LE(32)] = 255; },
    bytes => bytes.copy(bytes, 44, 32, 44)];
  for (const edit of edits) { const bytes = syntheticAtlas(); edit(bytes); assert.throws(() => parseAppleEmojiAtlas(bytes)); }
  for (const bytes of [Buffer.alloc(31), Buffer.alloc(2 * 1024 * 1024 + 1)]) assert.throws(() => parseAppleEmojiAtlas(bytes));
});

test('KEMJ permits shared bitmap offsets without sharing keys', () => {
  const bytes = syntheticAtlas(); bytes.writeUInt32LE(bytes.readUInt32LE(36), 48);
  const entries = [...parseAppleEmojiAtlas(bytes).values()];
  assert.deepEqual(entries[0], entries[1]);
});

test('private atlas cache invalidates on replacement, deletion, corruption and oversized files', t => {
  const f = fixture(t), first = f.atlas.load();
  assert.equal(f.atlas.load(), first);
  writeFileSync(f.path, syntheticAtlas(['😀'])); assert.notEqual(f.atlas.load(), first);
  writeFileSync(f.path, Buffer.from('bad')); assert.equal(f.atlas.load(), null);
  writeFileSync(f.path, Buffer.alloc(2 * 1024 * 1024 + 1)); assert.equal(f.atlas.load(), null);
  writeFileSync(f.path, syntheticAtlas()); assert.ok(f.atlas.load());
  rmSync(f.path); assert.equal(f.atlas.load(), null);
  const other = join(f.stateDir, 'other.bin'); writeFileSync(other, syntheticAtlas());
  symlinkSync(other, f.path); assert.equal(f.atlas.load(), null);
});

test('wire enrichment transmits only unique exact whole emoji, canonical 128-byte bitmaps', t => {
  const f = fixture(t);
  const wire = preparePrinterRequest(print('Bread 😀 😀 🇷🇺 ❤️ 👨‍👩‍👧‍👦 👍🏽 1️⃣'), f.atlas);
  assert.deepEqual(wire.emoji.map(item => item.sequence), ['😀', '🇷🇺', '❤️', '👨‍👩‍👧‍👦', '👍🏽', '1️⃣']);
  for (const item of wire.emoji) {
    assert.equal(item.bitmap.length, 172); assert.equal(Buffer.from(item.bitmap, 'base64').length, 128);
    assert.equal(Buffer.from(item.bitmap, 'base64').toString('base64'), item.bitmap);
  }
  assert.equal(wire.text, 'Bread 😀 😀 🇷🇺 ❤️ 👨‍👩‍👧‍👦 👍🏽 1️⃣');
});

test('unsupported composite or presentation never falls back to a constituent emoji', t => {
  const f = fixture(t, ['👨', '👩', '👧', '👦', '👍', '❤', '🇷', '🇺', '1']);
  for (const sequence of ['👨‍👩‍👧‍👦', '👍🏽', '❤️', '❤︎', '🇷🇺', '1️⃣', '😀']) {
    assert.throws(() => preparePrinterRequest(print(sequence), f.atlas), errorCode('unsupported_character'), sequence);
  }
});

test('ordinary text, status and QR need no atlas and never gain an unsolicited emoji field', () => {
  let loads = 0; const atlas = { load() { loads++; return null; } };
  assert.deepEqual(preparePrinterRequest(print('Привет\nBread'), atlas).emoji, []);
  const status = { t: 'printer', action: 'status', rid: 'b'.repeat(32) };
  assert.deepEqual(preparePrinterRequest(status, atlas), status);
  const qr = { ...print('😀 https://example.com'), format: 'qr' };
  assert.deepEqual(preparePrinterRequest(qr, atlas), qr); assert.equal(loads, 0);
  assert.throws(() => preparePrinterRequest(print('😀'), atlas), errorCode('unsupported_character'));
  assert.throws(() => preparePrinterRequest({ ...status, emoji: [] }, atlas), errorCode('invalid_request'));
});

test('at most eight distinct whole glyphs are admitted, repetitions cost one slot', t => {
  const sequences = Array.from({ length: 9 }, (_, i) => String.fromCodePoint(0x1f600 + i));
  const f = fixture(t, sequences);
  assert.equal(preparePrinterRequest(print(sequences.slice(0, 8).join('')), f.atlas).emoji.length, 8);
  assert.equal(preparePrinterRequest(print(sequences[0].repeat(100)), f.atlas).emoji.length, 1);
  assert.throws(() => preparePrinterRequest(print(sequences.join('')), f.atlas), errorCode('invalid_request'));
});
