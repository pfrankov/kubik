import { out } from './protocol.js';
import { randomBytes } from 'node:crypto';
import { createAccountListHelpers, normalizeAccountId, resolveMergedAccountConfig } from 'openclaw/plugin-sdk/account-core';

const KEY = Symbol.for('openclaw.kubik.telegram-pairing');
const TTL = 10 * 60_000;
const LIMIT = 4;
const numeric = (value) => /^[1-9]\d{0,19}$/.test(String(value));

// Use the same generic SDK account primitives as the native Telegram channel.
// Resolve policy without loading a bot token or a channel's internal modules.
const { listAccountIds } = createAccountListHelpers('telegram', {
  normalizeAccountId,
  implicitDefaultAccount: { channelKeys: ['botToken', 'tokenFile'], envVars: ['TELEGRAM_BOT_TOKEN'] },
  additionalAccountIds: (cfg) => (cfg.bindings ?? []).flatMap(({ match }) =>
    String(match?.channel ?? '').trim().toLowerCase() === 'telegram' &&
    typeof match.accountId === 'string' && match.accountId.trim() && match.accountId.trim() !== '*'
      ? [normalizeAccountId(match.accountId)] : []),
});

function privateButtons(capabilities) {
  if (Array.isArray(capabilities)) {
    return !capabilities.length || capabilities.some((item) => String(item).trim().toLowerCase() === 'inlinebuttons');
  }
  const scope = String(capabilities?.inlineButtons ?? 'allowlist').trim().toLowerCase();
  return ['allowlist', 'dm', 'all'].includes(scope);
}

/** Only explicitly configured Telegram owners receive private security prompts. */
export function telegramOwners(cfg) {
  const telegram = cfg.channels?.telegram;
  if (!telegram || telegram.enabled === false) return [];
  const owners = (cfg.commands?.ownerAllowFrom ?? []).map(String)
    .filter((id) => /^telegram:[1-9]\d{0,19}$/.test(id)).map((id) => id.slice(9));
  return listAccountIds(cfg).flatMap((accountId) => {
    const policy = resolveMergedAccountConfig({ channelConfig: telegram, accounts: telegram.accounts,
      accountId, normalizeAccountId, omitKeys: ['defaultAccount'],
      inheritEmptyKeys: { capabilities: 'array' }, preserveRootAllowFrom: true });
    if (policy.enabled === false || policy.dmPolicy === 'disabled' || !privateButtons(policy.capabilities)) return [];
    const allowed = (policy.allowFrom ?? []).map((id) => String(id).trim().replace(/^(telegram:|tg:)/, ''));
    return [...new Set(owners)].filter((owner) => allowed.includes(owner) || allowed.includes('*'))
      .map((owner) => ({ owner, accountId }));
  }).slice(0, LIMIT);
}

// Adapters may ignore signal. Bound our wait as well as requesting cancellation;
// a late resolution must never publish delivery state outside this operation.
async function bounded(operation, timeoutMs) {
  const controller = new AbortController();
  let timer;
  try {
    return await Promise.race([
      Promise.resolve().then(() => operation(controller.signal)),
      new Promise((_, reject) => { timer = setTimeout(() => {
        controller.abort();
        reject(Error('Telegram pairing operation timed out'));
      }, timeoutMs); }),
    ]);
  } finally { clearTimeout(timer); }
}

const shellWord = (value) => /^[a-zA-Z0-9_-]+$/.test(value) ? value : `'${value.replaceAll("'", "'\\''")}'`;
const approvalCommand = ({ accountId, code }) => accountId === 'default'
  ? `openclaw pairing approve kubik ${code}`
  : `openclaw pairing approve --channel kubik --account ${shellWord(accountId)} ${code}`;

export class TelegramPairing {
  constructor(api, { now = Date.now, token = () => randomBytes(12).toString('hex'), command, timeoutMs = 5000 } = {}) {
    Object.assign(this, { api, now, token, command, timeoutMs });
    this.requests = new Map();
    this.inFlight = new Map();
  }
  config() { return this.api.runtime.config?.current?.() ?? this.api.config; }
  authorized(target) {
    return telegramOwners(this.config()).some((item) => item.owner === target.owner && item.accountId === target.accountId);
  }
  live(key, request) { return this.requests.get(key) === request && request.expires > this.now(); }
  delivered(key, request) { return this.live(key, request) && request.targets.some((target) => this.authorized(target)); }
  async notify(accountId, identity, code) {
    if (!/^[A-Z2-9]{8}$/.test(code)) return false;
    const key = `${accountId}:${code}`;
    const now = this.now();
    for (const [id, request] of this.requests) if (request.expires <= now) this.requests.delete(id);
    const existing = this.requests.get(key);
    if (existing) {
      if (existing.identity !== identity) return false;
      return this.inFlight.get(key) ?? this.delivered(key, existing);
    }
    if (this.requests.size >= LIMIT || this.inFlight.size >= LIMIT) return false;
    const targets = telegramOwners(this.config());
    if (!targets.length) return false;
    const request = { accountId, identity, code, token: this.token(), expires: now + TTL, targets: [] };
    this.requests.set(key, request); // reserve before any await; failures also deduplicate reconnects
    const task = Promise.resolve().then(() => this.deliver(key, request, targets)).catch(() => false);
    this.inFlight.set(key, task);
    try { return await task; } finally { if (this.inFlight.get(key) === task) this.inFlight.delete(key); }
  }
  async deliver(key, request, targets) {
    const adapter = await bounded(() => this.api.runtime.channel.outbound.loadAdapter('telegram'), this.timeoutMs);
    if (!adapter?.sendPayload) return false;
    for (const target of targets) {
      if (!this.live(key, request)) break;
      if (this.authorized(target)) await this.send(adapter, key, request, target);
    }
    return this.delivered(key, request);
  }
  async send(adapter, key, request, target) {
    const text = `Connect ${request.identity.split(':')[0]}?\nMatch this code on Kubik: ${request.code}\nExpires within 10 minutes.`;
    try {
      const delivery = await bounded((signal) => adapter.sendPayload({ cfg: this.config(), to: target.owner,
        accountId: target.accountId, text, signal,
        payload: { text, presentation: { blocks: [{ type: 'buttons', buttons: [
          { label: 'Allow', action: { type: 'callback', value: `kubik-pair:${request.token}` } },
        ] }] } },
      }), this.timeoutMs);
      if (!numeric(delivery?.messageId)) throw Error('Telegram did not return a message identity');
      if (this.live(key, request) && this.authorized(target)) {
        request.targets.push({ ...target, messageId: String(delivery.messageId) });
      }
    } catch { this.api.logger?.warn?.('kubik: Telegram pairing delivery unavailable; use server approval'); }
  }
  rejectReason(ctx, request, owner, target) {
    if (!request || !target) return 'request';
    if (ctx.channel !== 'telegram' || !ctx.auth?.isAuthorizedSender || !numeric(owner)) return 'authorization';
    if (ctx.isGroup || ctx.isForum || ctx.threadId != null || String(ctx.callback.chatId) !== owner) return 'private-chat';
    if (String(ctx.callback.messageId) !== target.messageId) return 'message';
    if (request.expires <= this.now()) return 'expired';
    if (!this.authorized(target)) return 'owner-revoked';
    return null;
  }
  async respond(ctx, method, payload) {
    try { await bounded(() => ctx.respond[method](payload), this.timeoutMs); }
    catch { this.api.logger?.warn?.('kubik: Telegram pairing feedback unavailable'); }
  }
  async handle(ctx) {
    const token = String(ctx.callback?.payload ?? '');
    const found = [...this.requests].find(([, item]) => item.token === token);
    const request = found?.[1], owner = String(ctx.senderId ?? '');
    const target = request?.targets.find((item) => item.owner === owner && item.accountId === ctx.accountId);
    const rejected = this.rejectReason(ctx, request, owner, target);
    if (rejected) {
      this.api.logger?.warn?.(`kubik: Telegram approval rejected (${rejected})`);
      await this.respond(ctx, 'reply', { text: 'This request is expired or is not yours.' });
      return { handled: true };
    }
    this.requests.delete(found[0]); // consume before any awaited effect
    let result;
    try { result = await this.command(request); } catch { /* Never expose command diagnostics. */ }
    if (result?.code !== 0) {
      await this.respond(ctx, 'reply', { text: `Pairing was not approved. On your OpenClaw server run: ${approvalCommand(request)}` });
    } else {
      await this.respond(ctx, 'editMessage', { text: `Kubik approved. Code: ${request.code}`, buttons: [] });
    }
    return { handled: true }; // feedback failure must not turn a security callback into an agent message
  }
}

export function registerTelegramPairing(api) {
  const command = ({ accountId, code }) => api.runtime.system.runCommandWithTimeout(
    ['openclaw', 'pairing', 'approve', '--channel', 'kubik', '--account', accountId, code],
    { timeoutMs: 10_000, maxOutputBytes: 2048 });
  const pairing = globalThis[KEY] ??= new TelegramPairing(api, { command });
  pairing.api = api;
  pairing.command = command;
  api.registerInteractiveHandler({ channel: 'telegram', namespace: 'kubik-pair',
    handler: (ctx) => telegramPairing().handle(ctx) });
}

export const telegramPairing = () => globalThis[KEY];

/** Sending Telegram must never delay the device challenge or its pairing poll. */
export function sendPairingChallenge(pairing, conn, code) {
  conn.ws.send(JSON.stringify(out.pair(code)));
  Promise.resolve().then(() => pairing.notify?.(conn.entryId, code)).then((delivered) => {
    if (delivered && conn.phase === 'pending') conn.ws.send(JSON.stringify(out.pair(code, true)));
  }).catch(() => {}); // notification is optional; server/CLI approval is already displayed
}
