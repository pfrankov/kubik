import { CHANNEL_ID, DEFAULT_ACCOUNT_ID } from './config.js';
import { setupPlugin } from './channel-setup.js';
import { authHeaders } from './engines/common.js';
import { getServer, monitorAccount, transportStatus } from './monitor.js';
import { getRuntime } from './runtime.js';
import { normalizeTarget, sendPayload } from './send.js';
import { EMOTIONS } from './speech.js';
import { resolveOpenClawVoiceCapabilities } from './engines/openclaw.js';
import { resolveNativeVoice } from './native-voice.js';

export const SPEECH_RULES = [
  'Everything you write is converted to speech and spoken aloud by Kubik, a small cute desk character. Its screen shows an animated face.',
  'When the user must see something exactly (a code, a number to copy, an address, a link, a short list), put it between [[show]] and [[/show]]: it appears as text on Kubik\'s screen and is not spoken. Also say one short sentence about it aloud. Use it rarely.',
  'Answer in Russian unless the user speaks another language; then answer in that language.',
  'Keep it short and conversational: usually one to three short sentences, unless the user explicitly asks for more detail.',
  'Outside [[show]] blocks never use markdown, lists, headings, tables, code, URLs, file paths or emoji; they cannot be spoken. Describe things in words instead.',
  'Write numbers, dates, times, units and abbreviations the way they should be read aloud (for example "двадцать три градуса", "в половине восьмого").',
  `You may put one emotion tag right before a sentence to change Kubik's face, for example "[[happy]] Отличная новость!". Allowed tags: ${EMOTIONS.join(', ')}. Tags are never spoken; use them sparingly and never invent new ones.`,
];

/** Voice provider check plus the device transports (LAN address/port/key, Gateway route). */
export async function probeAccount({ account, cfg, timeoutMs = 10000 }, nativeResolver = resolveNativeVoice) {
  const { lan, gatewayRoute } = transportStatus(account.accountId);
  return { ...await probeVoice({ account, cfg, timeoutMs, nativeResolver }), lan, gatewayRoute };
}

async function probeVoice({ account, cfg, timeoutMs, nativeResolver }) {
  const start = Date.now();
  if (account.voice.provider === 'openclaw') {
    let core;
    try { core = getRuntime().core; } catch { /* not running inside the Gateway */ }
    const current = cfg ?? core?.config?.current?.() ?? { channels: { kubik: account.config } };
    const native = await nativeResolver({ cfg: current, voice: account.voice });
    const { stt, tts } = native ?? await resolveOpenClawVoiceCapabilities({ core, cfg: current, voice: account.voice });
    return { ok: stt.available, elapsedMs: Date.now() - start, provider: 'openclaw', stt, tts,
      ...(!stt.available ? { error: native ? 'Native voice is unavailable; check Gateway OpenAI Platform authorization and SDK' :
        'Speech recognition is not configured; text notifications and agent settings remain available' } : {}),
      ...(!tts.available ? { warning: native ? 'Native voice replies are unavailable; check Gateway OpenAI Platform authorization and SDK' :
        'Speech synthesis is not configured: replies are shown as text' } : {}) };
  }
  try {
    const response = await fetch(`${account.voice.baseUrl}/models`, { headers: authHeaders(account.voice), signal: AbortSignal.timeout(timeoutMs) });
    await response.body?.cancel();
    if (!response.ok) return { ok: false, elapsedMs: Date.now() - start, status: response.status,
      error: response.status === 401 ? 'Voice provider rejected the API key' : `Voice provider returned HTTP ${response.status}` };
    return { ok: true, elapsedMs: Date.now() - start, provider: account.voice.provider, baseUrl: account.voice.baseUrl };
  } catch {
    return { ok: false, elapsedMs: Date.now() - start, error: 'Voice provider is unreachable; check voice.baseUrl, network and TLS' };
  }
}

export const channelPlugin = {
  ...setupPlugin,
  // Device keys are approved with `openclaw pairing approve kubik <CODE>`; entries are `<deviceId>:<key fingerprint>`.
  // notifyApproval only runs for Gateway/Control UI approvals; CLI approvals are picked up by the server's poll.
  pairing: { ...setupPlugin.pairing, notifyApproval: async () => { await getServer()?.checkPairings(); } },
  agentPrompt: {
    inboundFormattingHints: () => ({ text_markup: 'plain_speech', rules: SPEECH_RULES }),
    messageToolHints: () => [
      'Kubik: message(action=send) to target kubik:<deviceId> delivers a notification. The device may speak audio or show text depending on voice availability and its settings.',
      'If the device is offline, an approved message is queued for the next connection, so delivery is not immediate. Plain short sentences only; emotion tags like [[happy]] are allowed.',
      'Kubik cannot show images or files.',
    ],
  },
  messaging: {
    normalizeTarget,
    parseExplicitTarget: ({ raw }) => (normalizeTarget(raw) ? { to: normalizeTarget(raw), chatType: 'direct' } : null),
    inferTargetChatType: ({ to }) => (normalizeTarget(to) ? 'direct' : undefined),
    targetResolver: { looksLikeId: (id) => Boolean(normalizeTarget(id)), hint: '<deviceId>, e.g. kubik-b6c634' },
  },
  // Voice replies should start after the first sentence or two, not after a whole paragraph.
  streaming: { blockStreamingCoalesceDefaults: { minChars: 60, idleMs: 200 } },
  outbound: {
    // The device socket lives in the Gateway process: CLI and tool sends are routed there.
    deliveryMode: 'gateway',
    textChunkLimit: 200, // also the block-streaming chunk size: long answers start speaking after ~200 chars
    chunker: null,
    shouldSkipPlainTextSanitization: () => true,
    sendPayload: ({ to, payload, ...options }) => sendPayload(to, payload, options),
    sendText: ({ to, text, ...options }) => sendPayload(to, { text }, options),
    sendMedia: ({ to, text, mediaUrl, ...options }) => sendPayload(to, { text, mediaUrl }, options),
  },
  security: {
    collectWarnings: ({ account }) => [
      ...(account.allowInsecureBaseUrl ? ['Kubik allowInsecureBaseUrl is set: the voice API key is sent over plain HTTP. Use it only with a local mock.'] : []),
    ],
  },
  status: {
    defaultRuntime: { accountId: DEFAULT_ACCOUNT_ID, running: false, connected: false, lastStartAt: null, lastStopAt: null, lastError: null },
    probeAccount,
    collectStatusIssues: (accounts) => accounts.filter((account) => !account.configured || account.lastError).map((account) => ({
      channel: CHANNEL_ID, accountId: account.accountId, kind: account.configured ? 'runtime' : 'config',
      message: account.configured ? account.lastError : 'Kubik is not enabled; run: openclaw channels add --channel kubik --enable',
    })),
    buildChannelSummary: ({ snapshot }) => ({ configured: snapshot.configured, running: snapshot.running,
      connected: snapshot.connected, onlineDevices: snapshot.onlineDevices ?? [], lastInboundAt: snapshot.lastInboundAt,
      lastOutboundAt: snapshot.lastOutboundAt, lastStartAt: snapshot.lastStartAt, lastStopAt: snapshot.lastStopAt,
      lastError: snapshot.lastError, mode: snapshot.mode, lan: snapshot.lan, gatewayRoute: snapshot.gatewayRoute, probe: snapshot.probe }),
    // `mode` is the one-line transport summary `channels status` prints (LAN address:port, key prefix, route).
    buildAccountSnapshot: ({ account, runtime, probe }) => ({ ...runtime, ...setupPlugin.config.describeAccount(account),
      mode: getServer(account.accountId) ? transportStatus(account.accountId).mode : runtime?.mode, insecureVoiceUrl: account.allowInsecureBaseUrl || undefined, probe }),
  },
  // A heartbeat (and a main-session cron job) that will report to a Kubik: shown as "thinking" there meanwhile.
  heartbeat: {
    sendTyping: async ({ to, accountId }) => { const id = normalizeTarget(to); if (id) getServer(accountId || DEFAULT_ACCOUNT_ID)?.setTyping(id, true); },
    clearTyping: async ({ to, accountId }) => { const id = normalizeTarget(to); if (id) getServer(accountId || DEFAULT_ACCOUNT_ID)?.setTyping(id, false); },
  },
  gateway: { startAccount: monitorAccount },
};
