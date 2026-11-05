import { createHash } from 'node:crypto';
import { PrinterError, validPrinterParameters } from './printer-contract.js';

const BROKER = Symbol.for('openclaw.kubik.printer-jobs');
const CACHE_MS = 10 * 60_000;
const CACHE_LIMIT = 64;
const TIMEOUT_MS = 5000;
const hash = (value) => createHash('sha256').update(value).digest('hex');
const validCallId = (value) => typeof value === 'string' && !!value.trim() && value.isWellFormed() && Buffer.byteLength(value) <= 256;

/** Cache outcomes instead of retrying side effects, including across authenticated reconnects. */
export class PrinterJobs {
  #records = new Map();
  #active = new Set();

  constructor(server, { timeoutMs = TIMEOUT_MS, now = Date.now } = {}) {
    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 10_000) throw new Error('Invalid printer timeout');
    Object.assign(this, { server, timeoutMs, now });
  }

  #prune() {
    const cutoff = this.now() - CACHE_MS;
    for (const [rid, record] of this.#records) {
      if (!record.uncertain && record.finishedAt !== null && record.finishedAt < cutoff) this.#records.delete(rid);
    }
  }

  #admit(device, rid, signal) {
    if (this.#records.size >= CACHE_LIMIT || this.#active.has(device.id)) throw new PrinterError('printer_busy', { rid });
    if (device.printer !== 1) throw new PrinterError('printer_unsupported', { rid });
    if (signal?.aborted) throw new PrinterError('cancelled', { rid });
  }

  #canSend(session, allowed) {
    const device = session.device;
    return allowed() && this.server.getSession(device.id) === session &&
      this.server.account.enabled && this.server.account.devices.get(device.id)?.enabled !== false;
  }

  #start(session, rid, parameters, payload, options) {
    const record = { payload, promise: null, finishedAt: null, uncertain: false };
    this.#records.set(rid, record);
    this.#active.add(session.device.id);
    record.promise = session.printer.request({ t: 'printer', rid, ...parameters }, options)
      .then((response) => this.#result(record, parameters, response))
      .catch((error) => { record.uncertain = error.outcome === 'unknown'; throw error; })
      .finally(() => { record.finishedAt = this.now(); this.#active.delete(session.device.id); });
    return record.promise;
  }

  #result(record, parameters, response) {
    const outcome = parameters.action !== 'print' ? undefined : response.status === 'sent' ? 'sent' :
      response.code === 'printer_failed' ? 'unknown' : 'not_sent';
    record.uncertain = outcome === 'unknown';
    return { rid: response.rid, status: response.status, ...(response.code ? { code: response.code } : {}),
      ...(outcome ? { outcome } : {}), paperConfirmed: false };
  }

  request(session, toolCallId, parameters, { signal, allowed = () => true } = {}) {
    if (!validPrinterParameters(parameters) || !validCallId(toolCallId)) return Promise.reject(new PrinterError('invalid_input'));
    this.#prune();
    const device = session?.device;
    if (!device || session.closed || this.server.getSession(device.id) !== session) return Promise.reject(new PrinterError('device_offline'));
    const rid = hash(`${device.id}\n${device.fingerprint}\n${toolCallId}`).slice(0, 32);
    const payload = hash(JSON.stringify([parameters.action, parameters.text ?? null]));
    const canSend = () => this.#canSend(session, allowed);
    try {
      if (!canSend()) throw new PrinterError('unauthorized', { rid });
      const previous = this.#records.get(rid);
      if (previous) return previous.payload === payload ? previous.promise : Promise.reject(new PrinterError('request_conflict', { rid }));
      this.#admit(device, rid, signal);
    } catch (error) { return Promise.reject(error); }
    return this.#start(session, rid, parameters, payload, { signal, allowed: canSend, timeoutMs: this.timeoutMs });
  }
}

export function printerJobs(server) {
  if (!server[BROKER]) Object.defineProperty(server, BROKER, { value: new PrinterJobs(server) });
  return server[BROKER];
}
