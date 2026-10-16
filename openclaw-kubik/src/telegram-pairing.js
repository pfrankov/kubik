import { out } from './protocol.js';
import { randomBytes } from 'node:crypto';

const KEY = Symbol.for('openclaw.kubik.telegram-pairing');
const TTL = 10 * 60_000;
const numeric = (value) => /^[1-9]\d{0,19}$/.test(String(value));

/** Only explicitly configured Telegram owners receive private security prompts. */
export function telegramOwners(cfg) {
  const telegram = cfg.channels?.telegram;
  if (!telegram || telegram.enabled === false) return [];
  const owners = (cfg.commands?.ownerAllowFrom ?? []).map(String)
    .filter((id) => /^telegram:[1-9]\d{0,19}$/.test(id)).map((id) => id.slice(9));
  const accounts = telegram.accounts ? Object.entries(telegram.accounts) : [['default', {}]];
  return accounts.filter(([, account]) => account.enabled !== false).flatMap(([accountId, account]) => {
    if ((account.dmPolicy ?? telegram.dmPolicy) === 'disabled') return [];
    const scope = (account.capabilities ?? telegram.capabilities)?.inlineButtons ?? 'allowlist';
    if (!['allowlist', 'dm', 'all'].includes(scope)) return [];
    const allowed = account.allowFrom ?? telegram.allowFrom ?? [];
    return [...new Set(owners)].filter((owner) => allowed.map(String).some((id) =>
      id === owner || id === `telegram:${owner}` || id === '*')).map((owner) => ({ owner, accountId }));
  }).slice(0, 4);
}

export class TelegramPairing {
  constructor(api, { now = Date.now, token = () => randomBytes(12).toString('hex'), command } = {}) {
    Object.assign(this, { api, now, token, command });
    this.requests = new Map();
    this.inFlight = new Map();
  }
  config() { return this.api.runtime.config?.current?.() ?? this.api.config; }
  async notify(accountId, identity, code) {
    const key = `${accountId}:${code}`;
    if (this.inFlight.has(key)) return this.inFlight.get(key);
    const task = this.deliver(key, accountId, identity, code).catch(() => false);
    this.inFlight.set(key, task);
    try { return await task; } finally { this.inFlight.delete(key); }
  }
  async deliver(key, accountId, identity, code) {
    if (!/^[A-Z2-9]{8}$/.test(code)) return false;
    const now = this.now();
    for (const [id, request] of this.requests) if (request.expires <= now) this.requests.delete(id);
    const existing = this.requests.get(key);
    if (existing) return existing.delivered;
    if (this.requests.size >= 4) return false;
    const cfg = this.config();
    const targets = telegramOwners(cfg);
    if (!targets.length) return false;
    const adapter = await this.api.runtime.channel.outbound.loadAdapter('telegram');
    if (!adapter?.sendPayload) return false;
    const request = { accountId, identity, code, token: this.token(), expires: now + TTL, delivered: false, targets: [] };
    this.requests.set(key, request); // also deduplicates failed delivery, avoiding reconnect spam
    for (const target of targets) {
      const text = `Connect ${identity.split(':')[0]}?\nMatch this code on Kubik: ${code}\nExpires in 10 minutes.`;
      try {
        const delivery = await adapter.sendPayload({ cfg, to: target.owner, accountId: target.accountId, text,
          payload: { text, presentation: { blocks: [{ type: 'buttons', buttons: [
            { label: 'Allow', action: { type: 'callback', value: `kubik-pair:${request.token}` } },
          ] }] } },
          signal: AbortSignal.timeout(5000) });
        if (!delivery?.messageId) throw Error('Telegram did not return a message identity');
        request.targets.push({ ...target, messageId: String(delivery.messageId) });
        request.delivered = true;
      } catch { this.api.logger?.warn?.('kubik: Telegram pairing delivery unavailable; use server approval'); }
    }
    return request.delivered;
  }
  rejectReason(ctx, request, owner, target) {
    if (!request || !target) return 'request';
    if (ctx.channel !== 'telegram' || !ctx.auth?.isAuthorizedSender || !numeric(owner)) return 'authorization';
    if (ctx.isGroup || ctx.isForum || ctx.threadId != null || String(ctx.callback.chatId) !== owner) return 'private-chat';
    if (String(ctx.callback.messageId) !== target.messageId) return 'message';
    if (request.expires <= this.now()) return 'expired';
    if (!telegramOwners(this.config()).some((item) => item.owner === owner && item.accountId === target.accountId)) return 'owner-revoked';
    return null;
  }
  async handle(ctx) {
    const token = String(ctx.callback?.payload ?? '');
    const found = [...this.requests].find(([, item]) => item.token === token);
    const request = found?.[1], owner = String(ctx.senderId ?? '');
    const target = request?.targets.find((item) => item.owner === owner && item.accountId === ctx.accountId);
    const rejected = this.rejectReason(ctx, request, owner, target);
    if (rejected) {
      this.api.logger?.warn?.(`kubik: Telegram approval rejected (${rejected})`);
      await ctx.respond.reply({ text: 'This request is expired or is not yours.' });
      return { handled: true };
    }
    this.requests.delete(found[0]); // consume before any awaited effect
    const result = await this.command(request).catch(() => ({ code: null }));
    if (result.code !== 0) {
      await ctx.respond.reply({ text: `Pairing was not approved. On your OpenClaw server run: openclaw pairing approve kubik ${request.code}` });
    } else {
      await ctx.respond.editMessage({ text: `Kubik approved. Code: ${request.code}`, buttons: [] });
    }
    return { handled: true }; // never submit text to the agent
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
