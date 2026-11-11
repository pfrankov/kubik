export const PRINTER_TOOL = 'kubik_printer';
export const PRINTER_TEXT_BYTES = 1024;
export const PRINTER_QR_BYTES = 256;
export const PRINTER_PROTOCOL_VERSION = 2;
export const PRINTER_RESULT_CODES = ['invalid_request', 'printer_busy', 'printer_not_found', 'printer_ambiguous',
  'printer_unsupported', 'printer_failed', 'printer_disconnected', 'content_does_not_fit', 'unsupported_character'];
export const PRINTER_STATES = ['ready', 'sent', 'error'];
export const printerRequestId = (value) => typeof value === 'string' && /^[0-9a-f]{32}$/.test(value);
const ERROR_BRAND = Symbol.for('openclaw.kubik.printer-error');
const HOST_ERROR_CODES = ['invalid_input', 'printer_busy', 'printer_unsupported', 'cancelled', 'device_offline',
  'request_conflict', 'unauthorized', 'device_replaced', 'disconnected', 'timeout', 'invalid_response',
  'invalid_request', 'unsupported_character'];

const record = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
const fields = (value, names) => record(value) && Object.keys(value).every((key) => names.includes(key));
const number = (value, min, max) => Number.isFinite(value) && value >= min && value <= max;
const integer = (value, min, max) => Number.isInteger(value) && number(value, min, max);
const measured = (mm, dots, dpi) => number(mm, 0, 216) && Math.abs(mm - dots * 25.4 / dpi) <= .01;
const label = (value) => typeof value === 'string' && value.isWellFormed() && !!value.trim() &&
  Buffer.byteLength(value, 'utf8') <= 64 && !/\p{C}/u.test(value);

function validPrinterGeometry(value) {
  return integer(value.dpi, 100, 600) && integer(value.feed_dpi, 100, 600) && integer(value.width_dots, 1, 4096) &&
    measured(value.printable_width_mm, value.width_dots, value.dpi) && number(value.max_height_mm, 1, 150) &&
    value.media_height_mm === null && value.size_source === 'model_profile';
}

export function validPrinterInfo(value) {
  if (!fields(value, ['name', 'model', 'firmware', 'hardware', 'dpi', 'feed_dpi', 'width_dots', 'printable_width_mm',
    'battery_pct', 'charging', 'formats', 'max_height_mm', 'media_height_mm', 'size_source'])) return false;
  return ['name', 'model', 'firmware', 'hardware'].every((key) => label(value[key])) &&
    validPrinterGeometry(value) && integer(value.battery_pct, 0, 100) && value.charging === null &&
    Array.isArray(value.formats) && value.formats.length === 2 &&
    value.formats.includes('text') && value.formats.includes('qr');
}

export function validPrinterLayout(value, printer) {
  if (!validPrinterInfo(printer) || !fields(value, ['format', 'width_dots', 'height_dots', 'width_mm', 'height_mm', 'font_mm'])) return false;
  return printer.formats.includes(value.format) && integer(value.width_dots, 1, printer.width_dots) &&
    integer(value.height_dots, 1, Math.floor(printer.max_height_mm * printer.feed_dpi / 25.4)) &&
    measured(value.width_mm, value.width_dots, printer.dpi) && measured(value.height_mm, value.height_dots, printer.feed_dpi) &&
    (value.font_mm === undefined || value.format === 'text' && number(value.font_mm, 1.5, 8));
}

function validResultStatus(message) {
  if (message.status === 'error') return PRINTER_RESULT_CODES.includes(message.code) &&
    (message.outcome === undefined || ['not_sent', 'unknown'].includes(message.outcome));
  if (message.code !== undefined || !validPrinterInfo(message.printer)) return false;
  return message.status === 'sent' ? message.outcome === 'submitted' && validPrinterLayout(message.layout, message.printer) :
    message.outcome === undefined && message.layout === undefined;
}

export function validPrinterResult(message) {
  if (!fields(message, ['t', 'rid', 'status', 'code', 'outcome', 'printer', 'layout']) ||
    message.t !== 'printer_result' || !printerRequestId(message.rid) || !PRINTER_STATES.includes(message.status)) return false;
  if (message.printer !== undefined && !validPrinterInfo(message.printer)) return false;
  if (message.layout !== undefined && !validPrinterLayout(message.layout, message.printer)) return false;
  return validResultStatus(message);
}

export function normalizePrinterResult(message) {
  return { t: 'printer_result', rid: message.rid, status: message.status,
    ...(message.code !== undefined ? { code: message.code } : {}),
    ...(message.outcome !== undefined ? { outcome: message.outcome } : {}),
    ...(message.printer ? { printer: { ...message.printer, formats: [...message.printer.formats] } } : {}),
    ...(message.layout ? { layout: { ...message.layout } } : {}) };
}

function validText(text, maximum) {
  return text.isWellFormed() && !!text.trim() && Buffer.byteLength(text, 'utf8') <= maximum &&
    !/[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/u.test(text);
}

function validPrintOptions(value, format) {
  return (value.width_mm === undefined || number(value.width_mm, 1, 216)) &&
    (value.height_mm === undefined || number(value.height_mm, 1, 150)) &&
    (value.font_mm === undefined || format === 'text' && number(value.font_mm, 1.5, 8));
}

export function validPrinterParameters(value) {
  if (!fields(value, ['action', 'text', 'format', 'width_mm', 'height_mm', 'font_mm'])) return false;
  if (value.action === 'status') return Object.keys(value).length === 1;
  if (value.action !== 'print' || typeof value.text !== 'string') return false;
  const format = value.format === undefined ? 'text' : value.format;
  return ['text', 'qr'].includes(format) && validText(value.text, format === 'qr' ? PRINTER_QR_BYTES : PRINTER_TEXT_BYTES) &&
    validPrintOptions(value, format);
}

export function canonicalPrinterParameters(value) {
  if (value.action === 'status') return { action: 'status' };
  return { action: 'print', text: value.text, format: value.format ?? 'text',
    ...Object.fromEntries(['width_mm', 'height_mm', 'font_mm'].filter((key) => value[key] !== undefined).map((key) => [key, value[key]])) };
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
