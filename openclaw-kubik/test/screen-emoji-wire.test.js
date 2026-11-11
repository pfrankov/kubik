import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import test from 'node:test';
import { start, talk } from './server-fixture.js';
import { connectDevice, DEFAULT_DEVICE_KEY, DEVICE } from './helpers.js';
import { syntheticColorAtlas } from './screen-emoji-fixture.js';

async function fixture(t) {
  const f = await start(t, { settings: { text: 'always', volume: 0 }, runtime: { payloads: [{ text: 'Привет 😀 👨‍👩‍👧‍👦!' }] } });
  writeFileSync(join(f.server.stateDir, 'apple-emoji-color.bin'), syntheticColorAtlas());
  const device = await connectDevice(f.url, { hello: DEFAULT_DEVICE_KEY.hello({ screen_emoji: 1 }), auth: nonce => DEFAULT_DEVICE_KEY.sign(nonce) });
  t.after(() => device.close()); return { ...f, device };
}

test('ordinary provider reply reaches its current card with full emoji and no extra model calls', async t => {
  const f = await fixture(t); talk(f.device, 1);
  const card = await f.device.waitFor(value => value.t === 'text' && value.text.includes('😀'));
  assert.ok(card.text.includes('👨‍👩‍👧‍👦')); assert.equal(f.seen.dispatches.length, 1);
  assert.equal(f.server.getSession(DEVICE).device.screen_emoji, 1);
  f.device.send({ t: 'emoji_get', id: 7, seq: ['😀', '👨‍👩‍👧‍👦', '❤️'] });
  const last = await f.device.waitFor(value => value.t === 'emoji' && value.id === 7 && value.seq === '❤️');
  assert.equal(last.data, null, 'private atlas entries outside the actual card are unavailable');
  const glyphs = f.device.events.filter(value => value.t === 'emoji' && value.id === 7 && value.data);
  assert.equal(glyphs.length, 2);
  for (const glyph of glyphs) { assert.equal(glyph.data.length, 768); assert.equal(Buffer.from(glyph.data, 'base64').length, 576); }
  await f.server.notify(DEVICE, '[[show]]Следующая ❤️[[/show]]');
  f.device.send({ t: 'emoji_get', id: 8, seq: ['😀', '❤️'] });
  const next = await f.device.waitFor(value => value.t === 'emoji' && value.id === 8 && value.seq === '❤️');
  assert.ok(next.data); assert.equal(f.device.events.find(value => value.t === 'emoji' && value.id === 8 && value.seq === '😀').data, null);
  assert.equal(f.seen.dispatches.length, 1);
});

test('emoji-only cards remain visible through the ordinary provider delivery path', async t => {
  const f = await fixture(t), session = f.server.getSession(DEVICE);
  await session.notify('😀');
  const card = await f.device.waitFor(value => value.t === 'text' && value.text.includes('😀'));
  assert.ok(card.text.includes('😀'));
  f.device.send({ t: 'emoji_get', id: 1, seq: ['😀'] });
  assert.ok((await f.device.waitFor(value => value.t === 'emoji' && value.id === 1)).data);
});

test('malformed screen requests close through the real protocol boundary without loading an atlas', async t => {
  const f = await fixture(t); let loads = 0; f.server.screenEmojiAtlas.load = async () => { loads++; return null; };
  f.device.send({ t: 'emoji_get', id: 1, seq: ['😀', '😀'] });
  assert.equal(await f.device.closed, 4002); assert.equal(loads, 0);
  assert.equal(f.device.events.some(value => value.t === 'emoji'), false);
});

test('revocation is checked before glyph delivery even on a previously welcomed socket', async t => {
  const f = await fixture(t); await f.server.getSession(DEVICE).notify('😀');
  const pairing = f.server.pairing; pairing.allowed = async () => [];
  f.device.send({ t: 'emoji_get', id: 1, seq: ['😀'] });
  assert.equal(await f.device.closed, 4001); assert.equal(f.device.events.some(value => value.t === 'emoji'), false);
});

test('a pre-welcome peer cannot fetch screen artwork', async t => {
  const f = await start(t), device = await connectDevice(f.url, { hello: DEFAULT_DEVICE_KEY.hello({ screen_emoji: 1 }), awaitAuth: false });
  t.after(() => device.close()); await device.waitFor(value => value.t === 'challenge');
  device.send({ t: 'emoji_get', id: 1, seq: ['😀'] });
  assert.equal(await device.closed, 4002); assert.equal(device.events.some(value => value.t === 'emoji'), false);
});
