import { config, connectDevice, deviceKey, fakePairing } from './helpers.js';
import { start } from './server-fixture.js';
import { PrinterJobs } from '../src/printer-jobs.js';
import { createPrinterToolFactory } from '../src/printer-tool.js';

export const printerInfo = () => ({ name: 'PeriPage+TEST', model: 'PeriPage A6+', firmware: 'V1.12_304dpi',
  hardware: 'v3.38.21_AY', dpi: 304, feed_dpi: 152, width_dots: 576, printable_width_mm: 576 * 25.4 / 304,
  battery_pct: 100, charging: null, formats: ['text', 'qr'], max_height_mm: 150, media_height_mm: null, size_source: 'model_profile' });
export const printerLayout = () => ({ format: 'text', width_dots: 240, height_dots: 60,
  width_mm: 240 * 25.4 / 304, height_mm: 60 * 25.4 / 152, font_mm: 3 });
export function printerResult(rid, status, { code, outcome, action = 'print', ...overrides } = {}) {
  return { t: 'printer_result', rid, status,
    ...(status === 'error' ? { code, ...(action === 'print' ? { outcome: outcome ?? 'not_sent' } : {}) } : { printer: printerInfo() }),
    ...(status === 'sent' ? { layout: printerLayout(), outcome: 'submitted' } : {}), ...overrides };
}

export async function printerFixture(t, { printer = 2, timeoutMs = 500, now, identity = deviceKey(), settings } = {}) {
  const pairing = fakePairing({ allowed: [identity.entry] });
  const { server, url } = await start(t, { pairing, settings, serverOptions: { pairingPollMs: 60_000 } });
  const options = { hello: identity.hello(printer === false ? {} : { printer }), auth: (nonce) => identity.sign(nonce) };
  const device = await connectDevice(url, options);
  t.after(() => device.close());
  const session = server.getSession(identity.device);
  const jobs = new PrinterJobs(server, { timeoutMs, ...(now ? { now } : {}) });
  return { server, url, device, session, pairing, identity, jobs, options };
}

export function toolFixture(server, deviceId, { cfg = config(), serverFor = () => server } = {}) {
  const state = { cfg };
  const core = { channel: { routing: { resolveAgentRoute: ({ accountId, peer }) => ({
    agentId: 'main', accountId, sessionKey: `agent:main:kubik:${accountId}:${peer.id}`,
  }) } } };
  const context = { messageChannel: 'kubik', nativeChannelId: deviceId, agentId: 'main', agentAccountId: 'default',
    sessionKey: `agent:main:kubik:default:${deviceId}` };
  const factory = createPrinterToolFactory({ currentConfig: () => state.cfg, runtime: () => ({ core }), serverFor });
  return { factory, context, state, tool: factory(context) };
}

export async function replyToPrinter(device, job, status, code) {
  const request = await device.waitFor((event) => event.t === 'printer' && event.rid === job);
  device.send(printerResult(request.rid, status, { code, action: request.action }));
  return request;
}
