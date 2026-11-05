import { config, connectDevice, deviceKey, fakePairing } from './helpers.js';
import { start } from './server-fixture.js';
import { PrinterJobs } from '../src/printer-jobs.js';
import { createPrinterToolFactory } from '../src/printer-tool.js';

export async function printerFixture(t, { printer = 1, timeoutMs = 500, now, identity = deviceKey(), settings } = {}) {
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
  device.send({ t: 'printer_result', rid: request.rid, status, ...(code ? { code } : {}) });
  return request;
}
