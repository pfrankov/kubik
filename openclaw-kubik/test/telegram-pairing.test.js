import test from 'node:test';
import assert from 'node:assert/strict';
import { TelegramPairing, telegramOwners, registerTelegramPairing, telegramPairing } from '../src/telegram-pairing.js';
import { sendPairingChallenge } from '../src/pairing-access.js';

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

function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
}

test('concurrent distinct requests reserve the four slots before loading the adapter', async () => {
  const f = fixture(), loaded = deferred();
  let loads = 0;
  f.api.runtime.channel.outbound.loadAdapter = () => { loads++; return loaded.promise; };
  const tasks = Array.from('ABCDEFGHIJKLMNOPQRST', (letter) => f.pairing.notify('default', `kubik-${letter}:key`, `AAAAAAA${letter}`));
  await new Promise(setImmediate);
  const pending = f.pairing.inFlight.size;
  loaded.resolve({ sendPayload: async () => ({ messageId: '7' }) });
  const delivered = await Promise.all(tasks);
  assert.ok(loads <= 4, `started ${loads} adapter loads`);
  assert.ok(pending <= 4, `retained ${pending} in-flight requests`);
  assert.equal(delivered.filter(Boolean).length, 4);
  assert.equal(f.pairing.requests.size, 4);
});

test('loader failure is deduplicated just like send failure', async () => {
  const f = fixture();
  let loads = 0;
  f.api.runtime.channel.outbound.loadAdapter = async () => { loads++; throw Error('offline'); };
  assert.equal(await notify(f), false);
  assert.equal(await notify(f), false);
  assert.equal(loads, 1);
});

test('cached delivery does not advertise revoked authority or another device identity', async () => {
  const f = fixture();
  await notify(f);
  assert.equal(await f.pairing.notify('default', 'kubik-other:key', 'ABCD2345'), false);
  f.cfg.commands.ownerAllowFrom = [];
  assert.equal(await notify(f), false);
  assert.equal(f.calls.length, 1);
});

test('expiry during adapter loading does not send a stale button', async () => {
  const f = fixture(), loaded = deferred();
  let sends = 0;
  f.api.runtime.channel.outbound.loadAdapter = () => loaded.promise;
  const task = notify(f);
  await new Promise(setImmediate);
  f.advance();
  loaded.resolve({ sendPayload: async () => { sends++; return { messageId: '7' }; } });
  assert.equal(await task, false);
  assert.equal(sends, 0);
});

test('ignored abort signals cannot hold a delivery slot or advertise a late result', async () => {
  const f = fixture(), sent = deferred();
  f.pairing.timeoutMs = 10;
  f.api.runtime.channel.outbound.loadAdapter = async () => ({ sendPayload: () => sent.promise });
  const task = notify(f);
  let watchdog;
  const result = await Promise.race([task, new Promise((done) => { watchdog = setTimeout(() => done('hung'), 100); })]);
  clearTimeout(watchdog);
  sent.resolve({ messageId: '7' });
  await task;
  assert.equal(result, false);
  assert.equal(f.pairing.inFlight.size, 0);
  await f.pairing.handle(f.ctx);
  assert.equal(f.approvals.length, 0);
});

test('approval while another owner delivery is pending prevents resurrection and further sends', async () => {
  const f = fixture(), second = deferred();
  f.cfg.commands.ownerAllowFrom = ['telegram:123', 'telegram:456', 'telegram:789'];
  f.cfg.channels.telegram.allowFrom = [123, 456, 789];
  let sends = 0;
  f.api.runtime.channel.outbound.loadAdapter = async () => ({ sendPayload: async () => {
    sends++;
    return sends === 2 ? second.promise : { messageId: '7' };
  } });
  const task = notify(f);
  await new Promise(setImmediate);
  await f.pairing.handle(f.ctx);
  second.resolve({ messageId: '8' });
  assert.equal(await task, false);
  assert.equal(sends, 2);
  assert.equal(f.approvals.length, 1);
  assert.equal(f.pairing.requests.size, 0);
});

test('synchronous and malformed command failures remain handled security callbacks', async () => {
  for (const command of [() => { throw Error('private'); }, async () => null]) {
    const f = fixture();
    await notify(f);
    f.pairing.command = command;
    assert.deepEqual(await f.pairing.handle(f.ctx), { handled: true });
    assert.match(f.replies[0].text, /not approved/);
    assert.doesNotMatch(f.replies[0].text, /private/);
  }
});

test('failed Telegram feedback cannot escape the callback or approve twice', async () => {
  const f = fixture();
  await notify(f);
  f.ctx.respond.editMessage = async () => { throw Error('message deleted'); };
  f.ctx.respond.reply = async () => { throw Error('chat unavailable'); };
  assert.deepEqual(await f.pairing.handle(f.ctx), { handled: true });
  assert.deepEqual(await f.pairing.handle(f.ctx), { handled: true });
  assert.equal(f.approvals.length, 1);
});

test('non-default account failure keeps the account in the recovery command', async () => {
  const f = fixture();
  await f.pairing.notify('office', 'kubik-test:fingerprint', 'ABCD2345');
  f.pairing.command = async () => ({ code: 1 });
  await f.pairing.handle(f.ctx);
  assert.match(f.replies[0].text, /--account office/);
});

test('named Telegram accounts inherit parent owners and direct-message policy', () => {
  const f = fixture();
  f.cfg.channels.telegram.accounts = { office: { botToken: 'unused' } };
  assert.deepEqual(telegramOwners(f.cfg), [{ owner: '123', accountId: 'office' }]);
  f.cfg.channels.telegram.dmPolicy = 'disabled';
  assert.deepEqual(telegramOwners(f.cfg), []);
  f.cfg.channels.telegram.accounts.office.dmPolicy = 'allowlist';
  assert.equal(telegramOwners(f.cfg).length, 1);
  f.cfg.channels.telegram.accounts.office.allowFrom = [];
  assert.deepEqual(telegramOwners(f.cfg), []);
});

test('an empty account map retains the implicit root Telegram account', async () => {
  const f = fixture();
  Object.assign(f.cfg.channels.telegram, { botToken: 'unused', accounts: {} });
  assert.deepEqual(telegramOwners(f.cfg), [{ owner: '123', accountId: 'default' }]);
  assert.equal(await notify(f), true);
  await f.pairing.handle(f.ctx);
  assert.equal(f.approvals.length, 1);
});

test('case-preserved account config delivers and approves with the native normalized account ID', async () => {
  const f = fixture();
  f.cfg.channels.telegram.accounts = { Office: { botToken: 'unused', allowFrom: [123] } };
  assert.deepEqual(telegramOwners(f.cfg), [{ owner: '123', accountId: 'office' }]);
  assert.equal(await notify(f), true);
  assert.equal(f.calls[0].accountId, 'office');
  await f.pairing.handle({ ...f.ctx, accountId: 'office' });
  assert.equal(f.approvals.length, 1);
  f.cfg.channels.telegram.accounts.Office.allowFrom = [];
  assert.deepEqual(telegramOwners(f.cfg), []);
});

test('native account listing retains the root bot alongside named and bound accounts', () => {
  const f = fixture();
  Object.assign(f.cfg.channels.telegram, { botToken: 'unused', accounts: { Office: {} } });
  assert.deepEqual(telegramOwners(f.cfg), [
    { owner: '123', accountId: 'default' }, { owner: '123', accountId: 'office' },
  ]);
  f.cfg.bindings = [{ match: { channel: 'telegram', accountId: 'Office' } },
    { match: { channel: 'telegram', accountId: 'Backup' } },
    { match: { channel: 'telegram', accountId: '*' } }, { match: { channel: 'other', accountId: 'other' } }];
  assert.deepEqual(telegramOwners(f.cfg), [
    { owner: '123', accountId: 'backup' }, { owner: '123', accountId: 'default' }, { owner: '123', accountId: 'office' },
  ]);
});

test('native capability arrays enable inlineButtons and reject other nonempty capabilities', () => {
  const f = fixture();
  for (const capabilities of [['inlineButtons'], [' INLINEBUTTONS '], []]) {
    f.cfg.channels.telegram.capabilities = capabilities;
    assert.equal(telegramOwners(f.cfg).length, 1);
  }
  for (const capabilities of [['reactions'], ['other', 'polls']]) {
    f.cfg.channels.telegram.capabilities = capabilities;
    assert.deepEqual(telegramOwners(f.cfg), []);
  }
});

test('empty per-account capability arrays inherit the parent scope', () => {
  const f = fixture();
  f.cfg.channels.telegram.capabilities = { inlineButtons: 'off' };
  f.cfg.channels.telegram.accounts = { office: { capabilities: [] } };
  assert.deepEqual(telegramOwners(f.cfg), []);
  f.cfg.channels.telegram.accounts.office.capabilities = ['inlineButtons'];
  assert.deepEqual(telegramOwners(f.cfg), [{ owner: '123', accountId: 'office' }]);
  f.cfg.channels.telegram.accounts.office.capabilities = { inlineButtons: 'group' };
  assert.deepEqual(telegramOwners(f.cfg), []);
});

test('a named-account wildcard cannot widen the restrictive parent allowlist', () => {
  const f = fixture();
  f.cfg.commands.ownerAllowFrom.push('telegram:456');
  f.cfg.channels.telegram.accounts = { office: { allowFrom: ['*'] } };
  assert.deepEqual(telegramOwners(f.cfg), [{ owner: '123', accountId: 'office' }]);
  f.cfg.channels.telegram.accounts.office.allowFrom = ['*', 456];
  assert.deepEqual(telegramOwners(f.cfg), [{ owner: '456', accountId: 'office' }]);
});

test('changing capability policy revokes cached delivery and outstanding callbacks', async () => {
  const f = fixture();
  await notify(f);
  f.cfg.channels.telegram.capabilities = ['reactions'];
  assert.equal(await notify(f), false);
  await f.pairing.handle(f.ctx);
  assert.equal(f.approvals.length, 0);
});
