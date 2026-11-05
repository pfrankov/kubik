import { DeviceSession } from './session.js';
import { PrinterError } from './printer-contract.js';

/** One bounded request on an already authenticated connection; no printer work runs in voice state. */
export class PrinterChannel {
  #pending = null;

  constructor(session) { this.session = session; }

  #allowed(operation) {
    const s = this.session;
    return !s.closed && s.current() === s && s.ws.readyState === 1 && operation.allowed();
  }

  #fail(operation, code) {
    operation.finish(new PrinterError(code, { rid: operation.request.rid,
      dispatched: operation.dispatched, action: operation.request.action }));
  }

  request(request, { signal, allowed, timeoutMs }) {
    if (this.#pending) return Promise.reject(new PrinterError('printer_busy'));
    if (signal?.aborted) return Promise.reject(new PrinterError('cancelled'));
    const operation = { request, allowed, dispatched: false, timer: null, abort: null, finish: null };
    const result = new Promise((resolve, reject) => {
      operation.finish = (error, response) => {
        if (this.#pending !== operation) return;
        this.#pending = null;
        clearTimeout(operation.timer);
        signal?.removeEventListener('abort', operation.abort);
        if (error) reject(error); else resolve(response);
      };
    });
    this.#pending = operation;
    operation.abort = () => this.#fail(operation, 'cancelled');
    signal?.addEventListener('abort', operation.abort, { once: true });
    operation.timer = setTimeout(() => this.#fail(operation, 'timeout'), timeoutMs);
    this.#dispatch(operation).catch(() => this.#fail(operation, 'unauthorized'));
    return result;
  }

  async #dispatch(operation) {
    if (!this.#allowed(operation)) { this.#fail(operation, 'device_replaced'); return; }
    const authorized = await this.session.authorize();
    if (this.#pending !== operation) return;
    if (!authorized) { this.#fail(operation, 'unauthorized'); return; }
    if (!this.#allowed(operation)) { this.#fail(operation, 'device_replaced'); return; }
    // A send that throws may have started an external effect; never retry it.
    operation.dispatched = true;
    try {
      if (!this.session.send(operation.request)) this.#fail(operation, 'disconnected');
    } catch { this.#fail(operation, 'disconnected'); }
  }

  accept(message) {
    const operation = this.#pending;
    if (!operation || !operation.dispatched || message.rid !== operation.request.rid) return false;
    if (!this.#allowed(operation)) { this.#fail(operation, 'device_replaced'); return false; }
    const invalid = operation.request.action === 'status' ? message.status === 'sent' : message.status === 'ready';
    if (invalid) { this.#fail(operation, 'invalid_response'); return false; }
    operation.finish(null, { ...message, paperConfirmed: false });
    return true;
  }

  close() { if (this.#pending) this.#fail(this.#pending, 'disconnected'); }
}

/** Keeps the ordinary voice session untouched while connecting its printer response lifecycle. */
export class PrinterDeviceSession extends DeviceSession {
  constructor(options) { super(options); this.printer = new PrinterChannel(this); }
  handleMessage(message) {
    if (message.t === 'printer_result') return this.printer.accept(message);
    return super.handleMessage(message);
  }
  close() { this.printer.close(); super.close(); }
}
