import test from 'node:test';
import assert from 'node:assert/strict';
import { TelegramPairing, telegramOwners, sendPairingChallenge, registerTelegramPairing, telegramPairing } from '../src/telegram-pairing.js';

function fixture() {
  let now = 1000;
  const calls = [], approvals = [], replies = [], edits = [];
  const cfg = { commands: { ownerAllowFrom: ['telegram:123', 'vk:999'] }, channels: { telegram: { enabled: true, allowFrom: [123] } } };
  const api = { config: cfg, runtime: { channel: { outbound: { loadAdapter: async () => ({
    sendPayload: async (payload) => { calls.push(payload); return { messageId: '7' }; },
  }) } } } };
  const pairing = new TelegramPairing(api, { now: () => now, token: () => '0123456789abcdef01234567',
    command: async (request) => { approvals.push(request); return { code: 0 }; } });
  const ctx = { channel: 'telegram', accountId: 'default', senderId: '123', isGroup: false, isForum: false,
    auth: { isAuthorizedSender: true }, callback: { payload: '0123456789abcdef01234567', messageId: 7, chatId: '123' },
    respond: { reply: async (data) => replies.push(data), editMessage: async (data) => edits.push(data) } };
  return { pairing, api, cfg, calls, approvals, replies, edits, ctx, advance: () => { now += 600_001; } };
}

const notify = (f) => f.pairing.notify('default', 'kubik-test:fingerprint', 'ABCD2345');

test('Telegram is optional; only explicit private Telegram owners receive prompts', () => {
  assert.deepEqual(telegramOwners({}), []);
  assert.deepEqual(telegramOwners({ channels: { telegram: {} }, commands: { ownerAllowFrom: ['*', '123', 'vk:123', 'telegram:-123'] } }), []);
  const f = fixture();
  assert.deepEqual(telegramOwners(f.cfg), [{ owner: '123', accountId: 'default' }]);
  f.cfg.channels.telegram.enabled = false;
  assert.deepEqual(telegramOwners(f.cfg), []);
});

test('delivery deduplicates reconnects; owner approval edits the exact prompt and never submits an agent turn', async () => {
  const f = fixture();
  assert.deepEqual(await Promise.all(Array.from({ length: 3 }, () => notify(f))), [true, true, true]);
  assert.equal(f.calls.length, 1);
  assert.equal(f.calls[0].payload.presentation.blocks[0].buttons[0].action.value, 'kubik-pair:0123456789abcdef01234567');
  assert.deepEqual(await f.pairing.handle(f.ctx), { handled: true });
  assert.match(f.edits[0].text, /approved/);
  assert.deepEqual(f.edits[0].buttons, []);
  await f.pairing.handle(f.ctx);
  assert.match(f.replies[0].text, /expired/);
  assert.equal(f.approvals.length, 1);
});

test('foreign user, bot account, channel, group, forum, thread and forwarded message are rejected', async () => {
  const f = fixture();
  await notify(f);
  for (const patch of [{ senderId: '456' }, { accountId: 'other' }, { channel: 'webchat' },
    { auth: { isAuthorizedSender: false } }, { isGroup: true }, { isForum: true }, { threadId: 7 },
    { callback: { ...f.ctx.callback, chatId: '456' } }, { callback: { ...f.ctx.callback, messageId: 8 } },
    { callback: { ...f.ctx.callback, payload: 'different' } }]) {
    await f.pairing.handle({ ...f.ctx, ...patch });
    assert.match(f.replies.at(-1).text, /not yours/);
  }
  f.cfg.commands.ownerAllowFrom = [];
  await f.pairing.handle(f.ctx);
  assert.equal(f.approvals.length, 0);
});

test('expiry and current owner/ingress authority are checked before effect', async () => {
  const f = fixture();
  await notify(f);
  f.cfg.channels.telegram.allowFrom = [];
  await f.pairing.handle(f.ctx);
  assert.equal(f.approvals.length, 0);
  f.cfg.channels.telegram.allowFrom = [123];
  f.advance();
  await f.pairing.handle(f.ctx);
  assert.equal(f.approvals.length, 0);
});

test('failed or unidentified delivery remains CLI, with bounded attempts', async () => {
  const f = fixture();
  let calls = 0;
  f.api.runtime.channel.outbound.loadAdapter = async () => ({ sendPayload: async () => { calls++; throw Error('offline'); } });
  assert.equal(await notify(f), false);
  assert.equal(await notify(f), false);
  assert.equal(calls, 1);
  await f.pairing.handle(f.ctx);
  assert.equal(f.approvals.length, 0);
});

test('device challenge is immediate; Telegram update only while still pending', async () => {
  let deliver;
  const frames = [], conn = { phase: 'pending', entryId: 'kubik:key', ws: { send: (text) => frames.push(JSON.parse(text)) } };
  const pairing = { notify: () => new Promise((resolve) => { deliver = resolve; }) };
  sendPairingChallenge(pairing, conn, 'ABCD2345');
  assert.deepEqual(frames, [{ t: 'pair', code: 'ABCD2345' }]);
  await new Promise(setImmediate); deliver(true); await new Promise(setImmediate);
  assert.deepEqual(frames[1], { t: 'pair', code: 'ABCD2345', approval: 'telegram' });
  sendPairingChallenge(pairing, conn, 'ABCD2345');
  await new Promise(setImmediate); conn.phase = 'ready'; deliver(true); await new Promise(setImmediate);
  assert.equal(frames.length, 3);
});

test('disabled direct messages, incompatible inline scope or missing ingress permission do not advertise a button', () => {
  const f = fixture();
  for (const scope of ['off', 'group']) {
    f.cfg.channels.telegram.capabilities = { inlineButtons: scope };
    assert.deepEqual(telegramOwners(f.cfg), []);
  }
  f.cfg.channels.telegram.capabilities = { inlineButtons: 'dm' };
  f.cfg.channels.telegram.dmPolicy = 'disabled';
  assert.deepEqual(telegramOwners(f.cfg), []);
  delete f.cfg.channels.telegram.dmPolicy;
  assert.equal(telegramOwners(f.cfg).length, 1);
  f.cfg.channels.telegram.allowFrom = [];
  assert.deepEqual(telegramOwners(f.cfg), []);
});

test('CLI failure offers the actual server command without exposing diagnostics', async () => {
  const f = fixture();
  await notify(f);
  f.pairing.command = async () => { throw Error('private provider credentials'); };
  await f.pairing.handle(f.ctx);
  assert.match(f.replies[0].text, /openclaw pairing approve kubik ABCD2345/);
  assert.doesNotMatch(f.replies[0].text, /private provider/);
});

test('multiple full registrations share the same delivery and callback state', async () => {
  const f = fixture(), registrations = [], argv = [];
  f.api.registerInteractiveHandler = (registration) => registrations.push(registration);
  f.api.runtime.system = { runCommandWithTimeout: async (args) => { argv.push(args); return { code: 0 }; } };
  delete globalThis[Symbol.for('openclaw.kubik.telegram-pairing')];
  try {
    registerTelegramPairing(f.api);
    const original = telegramPairing();
    original.token = () => f.ctx.callback.payload;
    registerTelegramPairing({ ...f.api });
    assert.equal(telegramPairing(), original);
    await telegramPairing().notify('default', 'kubik-test:fingerprint', 'ABCD2345');
    assert.equal(registrations[0].namespace, 'kubik-pair');
    await registrations[0].handler(f.ctx);
    assert.deepEqual(argv, [['openclaw', 'pairing', 'approve', '--channel', 'kubik', '--account', 'default', 'ABCD2345']]);
  } finally { delete globalThis[Symbol.for('openclaw.kubik.telegram-pairing')]; }
});
