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
      description: 'Inspect or print plain text on the printer physically connected to this conversation\'s Kubik. ' +
        'Print only when the user explicitly requests it. A sent result means submitted, never proof of paper output. ' +
        'If a print outcome is unknown, do not retry automatically. No printer is configured until its device driver is installed.',
      async execute(toolCallId, parameters, signal) {
        if (!binding.server || !binding.session) return toolResult({ status: 'error', code: 'device_offline', outcome: 'not_sent', paperConfirmed: false }, true);
        try {
          if (!allowed()) throw new PrinterError('device_replaced');
          const result = await printerJobs(binding.server).request(binding.session, toolCallId, parameters, { signal, allowed });
          const unavailable = parameters.action === 'print' && result.status === 'not_configured';
          return toolResult({ ...result, ...(unavailable ? { code: 'printer_not_configured' } : {}) }, unavailable || result.status === 'error');
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
    { name: PRINTER_TOOL, optional: true });
}
