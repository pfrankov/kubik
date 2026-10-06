import { CHANNEL_ID, DEFAULT_ACCOUNT_ID, isDeviceId, normalizeDeviceId } from './config.js';
import { speakablePayloadText } from './inbound.js';
import { getServer } from './monitor.js';
import { sanitizeForDisplay, speakableText } from './speech.js';

export class KubikDeliveryError extends Error {
  constructor(message, { retryable = false, attempted = false } = {}) {
    super(message);
    this.name = 'KubikDeliveryError';
    this.code = 'KUBIK_DELIVERY_FAILED';
    // Offline is worth retrying later; a failure after speech started must not be repeated.
    if (!retryable) this.noRetry = true;
    this.mayHaveSent = attempted;
  }
}

export function normalizeTarget(raw) {
  const id = normalizeDeviceId(raw);
  return isDeviceId(id) ? id : undefined;
}

/**
 * Proactive text through the shared server queue. `meta.deliveryStatus = queued` confirms acceptance
 * for an approved sleeping/offline device; `played` requires its playback ACK. Neither `shown`
 * (text card sent) nor `interrupted` implies audio finished. Queued ids are stable through reconnects.
 */
export async function speakNotification(to, text, { accountId, signal, server = getServer(accountId || DEFAULT_ACCOUNT_ID) } = {}) {
  const deviceId = normalizeTarget(to);
  if (!deviceId) throw new KubikDeliveryError(`Invalid Kubik target ${JSON.stringify(String(to ?? '').slice(0, 80))}; use kubik:<deviceId>`);
  if (!speakableText(text) && !sanitizeForDisplay(text)) throw new KubikDeliveryError('Nothing to say or show in the message (text is empty after removing markdown and emoji)');
  if (!server) throw new KubikDeliveryError('Kubik channel is not running in this process; send through the Gateway', { retryable: true });
  signal?.throwIfAborted();
  let result;
  try { result = await server.notify(deviceId, text, { signal }); }
  catch (error) {
    if (signal?.aborted) throw error;
    throw new KubikDeliveryError(error?.message ?? `Kubik device ${deviceId}: notification delivery failed`,
      { attempted: Boolean(error?.attempted), retryable: Boolean(error?.retryable) });
  }
  const timestamp = Date.now();
  return { channel: CHANNEL_ID, messageId: result.id, target: { kind: 'chat', id: deviceId },
    timestamp, meta: { deliveryStatus: result.status, spokenChars: result.spokenChars ?? 0,
      interrupted: Boolean(result.cancelled), ...(result.status === 'queued'
        ? { queuedAt: result.queuedAt, expiresAt: result.expiresAt, durable: result.durable } : {}) } };
}

export async function sendPayload(to, payload, options = {}) {
  const text = speakablePayloadText(payload, 'final');
  if (text === null) throw new KubikDeliveryError('Kubik can only speak text; the payload has no text');
  const result = await speakNotification(to, text, options);
  await options.onDeliveryResult?.(result);
  return result;
}
