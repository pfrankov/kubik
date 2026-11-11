import { DeviceSession } from './session.js';
import { PrinterError, validPrinterResult } from './printer-contract.js';

function matchesLayout(request, message) {
  if (message.status !== 'sent') return true;
  const layout = message.layout;
  const dot = 25.4 / message.printer.dpi;
  return layout.format === (request.format ?? 'text') &&
    ['width_mm', 'height_mm', 'font_mm'].every((key) => request[key] === undefined || layout[key] <= request[key] + dot);
}

/** One bounded request; uncertain remote cleanup retains output ownership after the caller stops waiting. */
export class PrinterChannel {
  #pending = null;

  constructor(session, { cleanupMs = 95_000 } = {}) {
    if (!Number.isInteger(cleanupMs) || cleanupMs < 1 || cleanupMs > 95_000) throw Error('Invalid printer cleanup deadline');
    Object.assign(this, { session, cleanupMs });
  }

  #allowed(operation) {
    const s = this.session;
    return !s.closed && s.current() === s && s.ws.readyState === 1 && operation.allowed();
  }

  #fail(operation, code) {
    if (this.#pending !== operation) return;
    operation.finish(new PrinterError(code, { rid: operation.request.rid,
      dispatched: operation.dispatched, action: operation.request.action }));
    if (!operation.dispatched) this.#release(operation);
  }

  #release(operation) {
    if (this.#pending !== operation) return;
    this.#pending = null;
    clearTimeout(operation.guard);
    operation.leaseAbort.abort();
    operation.release?.();
    operation.release = null;
  }

  request(request, { signal, allowed, timeoutMs }) {
    if (this.#pending) return Promise.reject(new PrinterError('printer_busy'));
    if (signal?.aborted) return Promise.reject(new PrinterError('cancelled'));
    const operation = { request, allowed, dispatched: false, timer: null, guard: null, abort: null, finish: null,
      settled: false, release: null, leaseAbort: new AbortController() };
    const result = new Promise((resolve, reject) => {
      operation.finish = (error, response) => {
        if (operation.settled) return;
        operation.settled = true;
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
    operation.release = await this.session.acquireOutputLease(operation.leaseAbort.signal);
    if (this.#pending !== operation) { operation.release(); return; }
    const authorized = await this.session.authorize();
    if (this.#pending !== operation) return;
    if (!authorized) { this.#fail(operation, 'unauthorized'); return; }
    if (!this.#allowed(operation)) { this.#fail(operation, 'device_replaced'); return; }
    // A send that throws may have started an external effect; never retry it.
    operation.dispatched = true;
    // Firmware closes BLE before replying and has a hard 90 s total operation bound.
    operation.guard = setTimeout(() => { this.#fail(operation, 'timeout'); this.#release(operation); }, this.cleanupMs);
    operation.guard.unref?.();
    try {
      if (!this.session.send(operation.request)) this.#fail(operation, 'disconnected');
    } catch { this.#fail(operation, 'disconnected'); }
  }

  accept(message) {
    const operation = this.#pending;
    if (!operation || !operation.dispatched || message.rid !== operation.request.rid) return false;
    const invalid = !validPrinterResult(message) || !matchesLayout(operation.request, message) || (operation.request.action === 'status' ?
      message.status === 'sent' || message.outcome !== undefined : message.status === 'ready' || message.outcome === undefined);
    if (invalid) { this.#fail(operation, 'invalid_response'); return false; }
    // A late, valid reply can prove cleanup without changing an already returned unknown outcome.
    if (!this.#allowed(operation)) { this.#fail(operation, 'device_replaced'); this.#release(operation); return false; }
    operation.finish(null, { ...message, paperConfirmed: false });
    this.#release(operation);
    return true;
  }

  close() {
    const operation = this.#pending;
    if (operation) { this.#fail(operation, 'disconnected'); this.#release(operation); }
  }
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
