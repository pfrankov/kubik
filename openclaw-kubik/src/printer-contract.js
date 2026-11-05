export const PRINTER_TOOL = 'kubik_printer';
export const PRINTER_TEXT_BYTES = 1024;
export const PRINTER_RESULT_CODES = ['invalid_request', 'printer_busy', 'printer_failed'];
export const PRINTER_STATES = ['not_configured', 'ready', 'sent', 'error'];
export const printerRequestId = (value) => typeof value === 'string' && /^[0-9a-f]{32}$/.test(value);
const ERROR_BRAND = Symbol.for('openclaw.kubik.printer-error');
const HOST_ERROR_CODES = ['invalid_input', 'printer_busy', 'printer_unsupported', 'cancelled', 'device_offline',
  'request_conflict', 'unauthorized', 'device_replaced', 'disconnected', 'timeout', 'invalid_response'];

export function validPrinterResult(message) {
  if (!printerRequestId(message.rid) || !PRINTER_STATES.includes(message.status)) return false;
  if (Object.keys(message).some((key) => !['t', 'rid', 'status', 'code'].includes(key))) return false;
  return message.status === 'error' ? PRINTER_RESULT_CODES.includes(message.code) : message.code === undefined;
}

export function validPrinterParameters(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  if (Object.keys(value).some((key) => !['action', 'text'].includes(key))) return false;
  if (value.action === 'status') return value.text === undefined;
  if (value.action !== 'print' || typeof value.text !== 'string') return false;
  return value.text.isWellFormed() && !!value.text.trim() && Buffer.byteLength(value.text, 'utf8') <= PRINTER_TEXT_BYTES &&
    !/[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/u.test(value.text);
}

export class PrinterError extends Error {
  constructor(code, { rid, dispatched = false, action } = {}) {
    super(code);
    if (!HOST_ERROR_CODES.includes(code)) throw new Error('Invalid printer error code');
    Object.defineProperty(this, ERROR_BRAND, { value: true });
    this.code = code;
    this.rid = rid;
    this.dispatched = dispatched;
    this.outcome = action === 'print' && dispatched ? 'unknown' : 'not_sent';
  }
}

/** Shared brokers may have been created by another loaded copy of the plugin. */
export function isPrinterError(value) {
  try {
    return value?.[ERROR_BRAND] === true && HOST_ERROR_CODES.includes(value.code) &&
      (value.rid === undefined || printerRequestId(value.rid)) && typeof value.dispatched === 'boolean' &&
      ['unknown', 'not_sent'].includes(value.outcome);
  } catch { return false; }
}
