import { CHANNEL_ID } from './config.js';
import { getRuntime } from './runtime.js';

export const MEDIA_NOTE = 'Мне прислали вложение, но показать его я не могу.';
const NON_SPEECH_FIELDS = ['isReasoning', 'isCommentary', 'isCompactionNotice', 'isStatusNotice', 'isFallbackNotice'];

// Session settings the user may change by voice. The voice model passes them to the agent verbatim; a spoken
// transcript never begins with "/", and none of these can touch anything beyond this device's own session.
const VOICE_COMMAND = /^\/(model|models|think|thinking|reasoning|fast|status|stop|new|reset)(\s|$)/i;
export function isVoiceCommand(text) { return VOICE_COMMAND.test(String(text ?? '').trim()); }

/** The agent asks the user something (ask_user / harness question): the payload carries its id. */
export function askUserQuestionId(payload) {
  const id = payload?.channelData?.askUser?.questionId;
  return typeof id === 'string' && id ? id : null;
}

/**
 * A question prompt as speech: the English framing ("Question for you:", "Reply with the number…") is
 * dropped, the options stay numbered so "второй" or the option itself answers it in the next turn.
 */
export function spokenQuestion(text) {
  const lines = String(text ?? '').split('\n').map((line) => line.trim()).filter(Boolean)
    .filter((line) => !/^(question for you|agent needs input):?$/i.test(line) && !/^reply (with|by)\b/i.test(line))
    .map((line) => (/^other: reply with your own answer\.?$/i.test(line) ? 'Или скажи свой вариант.' : line));
  return lines.length ? `Вопрос: ${lines.join('\n')}` : '';
}

/** Text a device can speak from one reply payload, or null when the payload is not for the user's ears. */
export function speakablePayloadText(payload, kind) {
  if (askUserQuestionId(payload)) return spokenQuestion(payload.text) || null;
  if (!payload || kind === 'tool') return null;
  if (NON_SPEECH_FIELDS.some((field) => payload[field])) return null;
  const text = [payload.text, payload.spokenText].find((value) => typeof value === 'string' && value.trim()) ?? '';
  const hasMedia = [payload.mediaUrl, payload.mediaUrls?.length].some(Boolean);
  if (!hasMedia) return text || null;
  return `${text}\n${MEDIA_NOTE}`.trim();
}

/** The device's own agent route (one session per device unless the operator configured something else). */
export function deviceRoute({ core, cfg, account, deviceId }) {
  cfg = { ...cfg, session: { ...cfg?.session, dmScope: cfg?.session?.dmScope ?? 'per-account-channel-peer' } };
  const route = core.channel.routing.resolveAgentRoute({ cfg, channel: CHANNEL_ID, accountId: account.accountId,
    peer: { kind: 'direct', id: deviceId } });
  return { cfg, route };
}

/**
 * Sends one exact transcript to the OpenClaw agent as a direct message from the device and feeds each
 * delivered reply block to `speak(text)` as it arrives. Only authenticated devices ever get here.
 */
export async function dispatchTranscript(options) {
  const runtime = getRuntime();
  return dispatchInbound({ ...runtime, ...options });
}

/** Sends a validated native model command through this device's normal channel ingress. */
export async function dispatchModelSelection({ core, sdk, account, cfg, device, modelId, log = () => {}, info = () => {},
  isPersisted, ackTimeoutMs = MODEL_ACK_TIMEOUT_MS }) {
  if (typeof modelId !== 'string' || !/^[^/\s]+\/[^\s]+$/.test(modelId) || /[\p{C}"'`]/u.test(modelId)) {
    throw Object.assign(new Error('invalid model command'), { code: 'invalid_model' });
  }
  let failed = false;
  const controller = new AbortController();
  const started = Date.now();
  const phase = (name, elapsed) => info(`kubik: model control ${device.id} ${name}${elapsed === undefined ? '' : ` ${elapsed}ms`}`);
  const dispatch = dispatchInbound({ core, sdk, account, cfg, device, transcript: `/model ${modelId} -s`,
    turnId: `model-${Date.now()}`, speak: () => false, onAgentError: () => { failed = true; },
    log, info: () => {}, setStatus: () => {}, commandAuthorized: true, commandSource: 'native', onPhase: phase,
    abortSignal: controller.signal });
  let settled = false, dispatchError;
  dispatch.then(() => { settled = true; }, (error) => { dispatchError = error; settled = true; });
  const deadline = started + Math.min(ackTimeoutMs, MODEL_ACK_TIMEOUT_MS);
  while (Date.now() < deadline) {
    if (isPersisted?.()) {
      phase('persisted', Date.now() - started);
      controller.abort();
      phase('abort-dispatch');
      return;
    }
    if (settled) {
      if (dispatchError) throw dispatchError;
      if (failed) throw Object.assign(new Error('model command was not accepted'), { code: 'unavailable' });
      if (isPersisted?.()) return;
      throw Object.assign(new Error('model command returned without persisting'), { code: 'unavailable' });
    }
    await delay(MODEL_ACK_POLL_MS);
  }
  controller.abort();
  phase('persistence-timeout', Date.now() - started);
  throw Object.assign(new Error('model selection was not persisted in time'), { code: 'timeout' });
}

const MODEL_ACK_TIMEOUT_MS = 6_500;
const MODEL_ACK_POLL_MS = 40;
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function dispatchInbound({ core, sdk, account, cfg, device, transcript, turnId, speak, onAgentError,
  log = () => {}, info = () => {}, setStatus = () => {}, commandAuthorized, commandSource, onPhase, abortSignal }) {
  const address = `${CHANNEL_ID}:${device.id}`;
  let route;
  let phaseStart = Date.now();
  onPhase?.('route-start');
  ({ cfg, route } = deviceRoute({ core, cfg, account, deviceId: device.id }));
  onPhase?.('route', Date.now() - phaseStart);
  const storePath = core.channel.session.resolveStorePath(cfg.session?.store, { agentId: route.agentId });
  const timestamp = Date.now();
  const ctx = core.channel.reply.finalizeInboundContext({
    Body: core.channel.reply.formatAgentEnvelope({ channel: 'Kubik', from: address, timestamp,
      previousTimestamp: core.channel.session.readSessionUpdatedAt({ storePath, sessionKey: route.sessionKey }),
      envelope: core.channel.reply.resolveEnvelopeFormatOptions(cfg), body: transcript }),
    BodyForAgent: transcript, RawBody: transcript, CommandBody: transcript,
    From: address, To: address, SessionKey: route.sessionKey, AccountId: account.accountId,
    ChatType: 'direct', ConversationLabel: device.name, SenderId: device.id, SenderName: device.name,
    Provider: CHANNEL_ID, Surface: CHANNEL_ID, OriginatingChannel: CHANNEL_ID, OriginatingTo: address,
    MessageSid: `${device.id}:${timestamp}:${turnId}`, Timestamp: timestamp, WasMentioned: true,
    // Speech is transcribed by a model; a misheard phrase must never run a privileged slash command. Only the
    // session settings above (model, reasoning, stop, new) run, when the voice model passes them on verbatim.
    CommandAuthorized: commandAuthorized ?? isVoiceCommand(transcript),
    ...(commandSource ? { CommandSource: commandSource } : {}),
    ...(commandSource === 'native' ? { CommandTargetSessionKey: route.sessionKey } : {}),
    NativeChannelId: device.id,
  });
  phaseStart = Date.now();
  onPhase?.('record-start');
  await core.channel.session.recordInboundSession({ storePath, ctx, sessionKey: route.sessionKey,
    onRecordError: () => log('kubik: session metadata could not be updated') });
  onPhase?.('record', Date.now() - phaseStart);
  setStatus({ lastInboundAt: Date.now() });
  const { onModelSelected, ...prefix } = sdk.replyPrefix({ cfg, agentId: route.agentId, channel: CHANNEL_ID, accountId: account.accountId });
  let failed = false;
  let blocks = 0;
  phaseStart = Date.now();
  onPhase?.('dispatch-start');
  await core.channel.reply.dispatchReplyWithBufferedBlockDispatcher({ ctx, cfg,
    dispatcherOptions: { ...prefix,
      deliver: async (payload, kind) => {
        if (payload?.isError) {
          log('kubik: agent returned an error payload; not reading it aloud');
          if (!failed) { failed = true; onAgentError?.(); }
          return;
        }
        const text = speakablePayloadText(payload, kind?.kind);
        if (text === null) return;
        const asking = Boolean(askUserQuestionId(payload));
        info(`kubik: ${device.id} turn ${turnId} reply ${asking ? 'question' : kind?.kind ?? 'block'} #${++blocks} (${text.length} chars)`);
        if (await speak(text, { asking })) setStatus({ lastOutboundAt: Date.now() });
      },
      onError: () => {
        log('kubik: reply delivery failed; inspect Gateway logs');
        if (!failed) { failed = true; onAgentError?.(); }
      },
    },
    // Voice needs the first sentence as early as possible: stream completed blocks.
    replyOptions: { onModelSelected, disableBlockStreaming: false, ...(abortSignal ? { abortSignal } : {}) } });
  onPhase?.('dispatch', Date.now() - phaseStart);
}
