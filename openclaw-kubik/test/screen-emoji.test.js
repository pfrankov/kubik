import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import test from 'node:test';
import { ColorEmojiAtlas, parseColorEmojiAtlas } from '../src/screen-emoji-atlas.js';
import { ScreenEmojiChannel } from '../src/screen-emoji-session.js';
import { parseDeviceMessage, ProtocolError } from '../src/protocol.js';
import { deviceKey, sleep } from './helpers.js';
import { COLOR_SEQUENCES, syntheticColorAtlas } from './screen-emoji-fixture.js';

const request = (id = 1, seq = ['😀']) => ({ t: 'emoji_get', id, seq });
const parse = value => parseDeviceMessage(Buffer.from(JSON.stringify(value)));

test('screen emoji capability and bounded whole sequences have one explicit protocol shape', () => {
  const key = deviceKey(); assert.equal(parse(key.hello({ screen_emoji: 1 })).screen_emoji, 1);
  assert.equal(parse(key.hello()).screen_emoji, undefined);
  for (const screen_emoji of [true, false, 0, 2, '1']) assert.throws(() => parse(key.hello({ screen_emoji })), ProtocolError);
  assert.deepEqual(parse(request(0xffffffff, COLOR_SEQUENCES.slice(0, 4))), request(0xffffffff, COLOR_SEQUENCES.slice(0, 4)));
  for (const bad of [{ id: 0 }, { id: 0x100000000 }, { id: 1.5 }, { id: '1' }, { seq: [] },
    { seq: COLOR_SEQUENCES.slice(0, 5) }, { seq: ['😀', '😀'] }, { seq: ['😀abc'] }, { seq: ['abc'] },
    { seq: ['\ufe0f'] }, { seq: ['\ud800'] }, { seq: ['😀\u200d'] }, { seq: ['😀\n'] }, { path: '/private' }]) {
    assert.throws(() => parse({ ...request(), ...bad }), ProtocolError);
  }
});

test('KEMC validates bounded index, UTF-8, palette transparency and all referenced indices', () => {
  const bytes = syntheticColorAtlas(), entries = parseColorEmojiAtlas(bytes);
  assert.equal(entries.size, 6); assert.equal([...entries.values()][0].length, 576);
  const edits = [b => b.write('KEMJ'), b => b.writeUInt32LE(2, 4), b => b.writeUInt32LE(16, 8),
    b => b.writeUInt32LE(0, 12), b => b.writeUInt32LE(8193, 12), b => b.writeUInt32LE(0, 20),
    b => b.writeUInt32LE(1, 24), b => b.writeUInt32LE(b.length + 1, 28), b => b.writeUInt16LE(1, 42),
    b => b.writeUInt16LE(65, 40), b => b.writeUInt32LE(0, 32), b => b.writeUInt32LE(b.length - 4, 36),
    b => { b[b.readUInt32LE(32)] = 255; }, b => { b[b.readUInt32LE(32)] = 0; },
    b => b.copy(b, 44, 32, 44), b => { b[b.readUInt32LE(36)] = 1; },
    b => { b[b.readUInt32LE(36) + 64] = 0x21; }, b => { b[b.readUInt32LE(36) + 64] = 0x12; },
    b => { b.set([9, 0, 0, 0], b.readUInt32LE(36) + 8); },
    b => { b.set([9, 0, 0, 255], b.readUInt32LE(36) + 8); }];
  for (const edit of edits) { const changed = Buffer.from(bytes); edit(changed); assert.throws(() => parseColorEmojiAtlas(changed)); }
  assert.throws(() => parseColorEmojiAtlas(Buffer.alloc(8 * 1024 * 1024 + 1)));
  const alias = Buffer.from(bytes); alias.writeUInt32LE(alias.readUInt32LE(36), 48);
  assert.deepEqual([...parseColorEmojiAtlas(alias).values()][0], [...parseColorEmojiAtlas(alias).values()][1]);
  assert.equal(createHash('sha256').update(bytes).digest('hex'), '649a4e4f1bfce244d4f0a60e6e26eb0ffe980d31bfb1787f667ae4a6b22b3170');
});

test('shared asynchronous color cache invalidates corrupted, removed or linked private files', async t => {
  const directory = mkdtempSync(join(tmpdir(), 'kubik-color-')); t.after(() => rmSync(directory, { recursive: true, force: true }));
  const path = join(directory, 'apple-emoji-color.bin'), atlas = new ColorEmojiAtlas(directory);
  writeFileSync(path, syntheticColorAtlas());
  const [a, b] = await Promise.all([atlas.load(), atlas.load()]); assert.equal(a, b); assert.equal(await atlas.load(), a);
  writeFileSync(path, Buffer.from('bad')); assert.equal(await atlas.load(), null);
  rmSync(path); assert.equal(await atlas.load(), null);
  const other = join(directory, 'other.bin'); writeFileSync(other, syntheticColorAtlas()); symlinkSync(other, path);
  assert.equal(await atlas.load(), null);
});

test('KEMC rejects an entirely transparent glyph and accepts a single visible pixel', () => {
  const bytes = syntheticColorAtlas(), glyph = bytes.readUInt32LE(36);
  bytes.fill(0, glyph, glyph + 576);
  assert.throws(() => parseColorEmojiAtlas(bytes), 'an all-zero glyph has no visible palette index');
  bytes.set([9, 40, 80, 255], glyph + 4); bytes[glyph + 64] = 0x10;
  assert.equal(parseColorEmojiAtlas(bytes).size, 6);
});

function channel({ authorize = async () => true, load, now = () => 0 } = {}) {
  const sent = [], calls = { load: 0 }; let current = true;
  const session = { closed: false, ws: { readyState: 1 }, current: () => current ? session : null, authorize,
    send: value => sent.push(value) };
  const entries = parseColorEmojiAtlas(syntheticColorAtlas());
  const handler = new ScreenEmojiChannel(session, { load: async () => { calls.load++; return load ? load() : entries; } }, { now });
  return { handler, sent, calls, session, replace: () => { current = false; } };
}

test('only the welcomed current card may load glyphs; missing and unrelated glyphs return null', async () => {
  const f = channel(); f.handler.card('😀 ❤️'); await f.handler.handle(request()); assert.equal(f.sent.length, 0);
  f.handler.start(); await f.handler.handle(request(1, ['😀', '🇷🇺']));
  assert.equal(Buffer.from(f.sent[0].data, 'base64').length, 576); assert.equal(f.sent[0].data.length, 768);
  assert.equal(f.sent[1].data, null); assert.equal(f.calls.load, 1);
  f.handler.card('No emoji'); await f.handler.handle(request(2)); assert.equal(f.sent.at(-1).data, null); assert.equal(f.calls.load, 1);
  const missing = channel({ load: () => null }); missing.handler.start(); missing.handler.card('😀');
  await missing.handler.handle(request()); assert.equal(missing.sent[0].data, null);
});

test('four request burst is bounded, sustained refill is two per second and overlimit is explicit null', async () => {
  let time = 0; const f = channel({ now: () => time }); f.handler.start(); f.handler.card('😀');
  for (let id = 1; id <= 4; id++) await f.handler.handle(request(id));
  assert.equal(f.calls.load, 4); await f.handler.handle(request(5)); assert.equal(f.calls.load, 4); assert.equal(f.sent.at(-1).data, null);
  time = 499; await f.handler.handle(request(6)); assert.equal(f.sent.at(-1).data, null);
  time = 500; await f.handler.handle(request(7)); assert.equal(f.calls.load, 5); assert.ok(f.sent.at(-1).data);
});

test('a blocked outbound socket cannot accumulate unbounded glyph or null replies', async () => {
  const f = channel(); f.handler.start(); f.handler.card('😀'); f.session.ws.bufferedAmount = 64 * 1024 + 1;
  for (let id = 1; id <= 20; id++) await f.handler.handle(request(id));
  assert.equal(f.calls.load, 0); assert.equal(f.sent.length, 0);
});

test('fast page changes coalesce one authorization/read job and never replay an older page', async () => {
  let release; const gate = new Promise(resolve => { release = resolve; });
  const f = channel({ load: () => gate }); f.handler.start(); f.handler.card('😀 ❤️');
  const work = f.handler.handle(request(1)); await sleep(0);
  f.handler.handle(request(2, ['❤️'])); release(parseColorEmojiAtlas(syntheticColorAtlas())); await work;
  assert.equal(f.calls.load, 1); assert.deepEqual(f.sent.map(value => [value.id, value.seq]), [[2, '❤️']]);
});

test('revoked, replaced, closed or removed-source requests cannot leak a late glyph', async t => {
  for (const action of ['revoke', 'replace', 'close', 'source']) await t.test(action, async () => {
    let release, permitted = true; const gate = new Promise(resolve => { release = resolve; });
    const f = channel({ authorize: async () => permitted, load: () => gate }); f.handler.start(); f.handler.card('😀');
    const work = f.handler.handle(request()); await sleep(0);
    if (action === 'revoke') permitted = false;
    if (action === 'replace') f.replace();
    if (action === 'close') f.handler.close();
    if (action === 'source') f.handler.card('New card');
    release(parseColorEmojiAtlas(syntheticColorAtlas())); await work; assert.equal(f.sent.length, 0);
  });
});
