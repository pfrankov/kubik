import { isDeviceId, resolveAccount } from './config.js';
import { deviceRoute } from './inbound.js';
import { getServer } from './monitor.js';
import { getRuntime } from './runtime.js';
import { PRINTER_TEXT_BYTES, PRINTER_TOOL, PrinterError, isPrinterError } from './printer-contract.js';
import { printerJobs } from './printer-jobs.js';

export const printerParameters = {
  type: 'object', additionalProperties: false, required: ['action'],
  properties: {
    action: { type: 'string', enum: ['status', 'print'], description: 'Inspect the printer, or print text requested by the user.' },
    text: { type: 'string', minLength: 1, maxLength: PRINTER_TEXT_BYTES,
      description: 'Required only for print. Plain text, at most 1024 UTF-8 bytes; tabs and line feeds are allowed.' },
    format: { type: 'string', enum: ['text', 'qr'], description: 'Print text by default, or a QR code (at most 256 UTF-8 bytes).' },
    width_mm: { type: 'number', minimum: 1, maximum: 216, description: 'Optional output width in millimetres, within the printable width returned by status.' },
    height_mm: { type: 'number', minimum: 1, maximum: 150, description: 'Optional height limit in millimetres. Content must fit without cropping.' },
    font_mm: { type: 'number', minimum: 1.5, maximum: 8, description: 'Optional text font height in millimetres; not used for QR codes.' },
  },
};

function contextBinding(context, cfg, core, serverFor) {
  if (context.messageChannel !== 'kubik' || !isDeviceId(context.nativeChannelId) || !context.agentId || !context.sessionKey) return null;
  const accountId = context.agentAccountId ?? 'default';
  const account = resolveAccount(cfg, accountId, { readSecrets: false });
  if (!account.enabled || account.devices.get(context.nativeChannelId)?.enabled === false) return null;
  const { route } = deviceRoute({ core, cfg, account, deviceId: context.nativeChannelId });
  if (route.sessionKey !== context.sessionKey || route.agentId !== context.agentId) return null;
  const server = serverFor(accountId);
  return { accountId, server, session: server?.getSession(context.nativeChannelId) };
}

function toolResult(details, isError = false) {
  return { content: [{ type: 'text', text: JSON.stringify(details) }], details, ...(isError ? { isError: true } : {}) };
}

/** The target comes from trusted channel routing, never from the model's arguments. */
export function createPrinterToolFactory({ currentConfig, runtime = getRuntime, serverFor = getServer } = {}) {
  return (context) => {
    let binding;
    try { binding = contextBinding(context, currentConfig(), runtime().core, serverFor); } catch { return null; }
    if (!binding) return null;
    const allowed = () => {
      try {
        const current = contextBinding(context, currentConfig(), runtime().core, serverFor);
        return !!current && current.server === binding.server && current.session === binding.session;
      } catch { return false; }
    };
    return {
      name: PRINTER_TOOL, label: 'Kubik printer', parameters: printerParameters,
      description: 'Inspect or print text and QR codes on the printer automatically found by this conversation\'s Kubik. ' +
        'Call status first for printable width, head DPI, feed DPI, battery percentage and supported formats; use both axes when sizing a job. ' +
        'Charging is unknown when charging is null; never infer it from the battery level. ' +
        'Media height is not sensed: do not guess the loaded label size. Width comes from the identified model profile. ' +
        'Text emoji require an exact glyph in the host private atlas; do not replace an unsupported emoji with another style. ' +
        'Print only when the user explicitly requests it. A sent result means submitted, never proof of paper output. ' +
        'Content must fit the requested dimensions without truncation. If a print outcome is unknown, do not retry automatically. ' +
        'In replies, briefly say the job was sent or give a plain reason why printing is unavailable. ' +
        'If the outcome is unknown, say so; do not claim success. Do not volunteer paper output, dimensions, resolution, ' +
        'format or other technical details; discuss them only if asked.',
      async execute(toolCallId, parameters, signal) {
        if (!binding.server || !binding.session) return toolResult({ status: 'error', code: 'device_offline', outcome: 'not_sent', paperConfirmed: false }, true);
        try {
          if (!allowed()) throw new PrinterError('device_replaced');
          const result = await printerJobs(binding.server).request(binding.session, toolCallId, parameters, { signal, allowed });
          return toolResult(result, result.status === 'error');
        } catch (error) {
          const known = isPrinterError(error);
          return toolResult({ status: 'error', code: known ? error.code : 'printer_failed',
            ...(known && error.rid ? { rid: error.rid } : {}), outcome: known ? error.outcome : 'unknown', paperConfirmed: false }, true);
        }
      },
    };
  };
}

export function registerPrinterTool(api) {
  api.registerTool(createPrinterToolFactory({ currentConfig: () => api.runtime?.config?.current?.() ?? api.config }),
    { name: PRINTER_TOOL, optional: false });
}
