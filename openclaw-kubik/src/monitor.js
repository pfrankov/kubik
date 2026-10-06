import { join } from 'node:path';
import { CHANNEL_ID, DEFAULT_ACCOUNT_ID } from './config.js';
import { ActivityTracker } from './activity.js';
import { createAgentControl } from './agent-control.js';
import { cronWatcher } from './cron.js';
import { createEngine } from './engines/index.js';
import { routeStatus } from './gateway-route.js';
import { deviceRoute, dispatchTranscript } from './inbound.js';
import { LanListener, deviceServerAddresses } from './lan.js';
import { getRuntime } from './runtime.js';
import { KubikServer } from './server.js';
import { loadTlsIdentity } from './tls-identity.js';

// Live device servers of this process, by account id. Outbound delivery looks devices up here. Shared through
// globalThis: the Gateway may load this module more than once (the channel runtime and the outbound delivery
// path of cron/heartbeat runs got separate instances), and every copy must see the same devices.
const REGISTRY = Symbol.for('openclaw.kubik.servers');
const servers = globalThis[REGISTRY] ??= new Map();
export const getServer = (accountId = DEFAULT_ACCOUNT_ID) => servers.get(accountId);

/** LAN listener and Gateway route state for `channels status` (`mode` is the one-line summary). */
export function transportStatus(accountId = DEFAULT_ACCOUNT_ID) {
  const lan = getServer(accountId)?.lan;
  const route = routeStatus();
  const status = lan?.listener?.status;
  const lanText = status ? `lan ${deviceServerAddresses(status).join(', ') || `(no reachable IPv4):${status.port}`}${status.public ? ' public' : ''} spki:${status.spki.slice(0, 16)}`
    : lan?.off ? 'lan off' : `lan unavailable${lan?.error ? ` (${lan.error})` : ''}`;
  return { mode: `${lanText}; route ${route.registered ? route.path : 'not registered'}`,
    lan: status ?? (lan?.off ? { enabled: false } : { error: lan?.error ?? 'not started' }), gatewayRoute: route };
}

/**
 * Starts the LAN TLS listener with the persistent identity from the plugin state dir. A failure (e.g. the port
 * is taken) is reported, not fatal: devices can still reach the Gateway route.
 */
async function startLan({ account, server, core, options = {}, warn }) {
  if (account.listen.enabled === false) {
    server.lan = { off: true };
    return null;
  }
  try {
    const dir = options.stateDir ?? join(core.state.resolveStateDir(process.env), CHANNEL_ID);
    const listener = new LanListener({ ...options, identity: loadTlsIdentity(dir, { log: warn }), port: account.listen.port,
      publicPeers: account.listen.public, log: warn,
      onUpgrade: (req, socket, head, transport) => server.handleUpgrade(req, socket, head, transport) });
    await listener.start();
    server.lan = { listener };
    return null;
  } catch (error) {
    const reason = error?.code === 'EADDRINUSE' ? `port ${account.listen.port} is already in use` : error?.message ?? String(error);
    server.lan = { error: reason };
    warn(`kubik: LAN listener is unavailable: ${reason}; devices can still connect through the Gateway route`);
    return `Kubik LAN listener is unavailable: ${reason}`;
  }
}

/** `gateway.startAccount`: runs the device WebSocket server until the Gateway aborts the account. */
export async function monitorAccount(ctx, deps = {}) {
  const account = ctx.account;
  if (!account.enabled) throw new Error('Kubik channel is disabled');
  const runtime = deps.core && deps.sdk ? undefined : getRuntime();
  const sdk = deps.sdk ?? runtime.sdk;
  const core = deps.core ?? runtime?.core;
  const signal = ctx.abortSignal;
  const setStatus = (patch) => ctx.setStatus?.({ accountId: account.accountId, ...patch });
  const log = (message) => ctx.log?.info?.(message);
  const warn = (message) => ctx.log?.warn?.(message);
  setStatus({ running: true, connected: false, lifecycle: 'starting', lastStartAt: Date.now(), lastError: null, onlineDevices: [] });
  if (account.allowInsecureBaseUrl) warn('kubik: allowInsecureBaseUrl is set; the voice API key travels in cleartext (use only with a local mock)');
  const pairing = pairingStore(core, account.accountId);
  if (!pairing) warn('kubik: the OpenClaw channel pairing API is unavailable; devices cannot connect until pairing is available');
  const agentControlFactory = deps.createAgentControl ?? createAgentControl;
  const agentControl = deps.agentControl ?? agentControlFactory({ core, sdk, cfg: ctx.cfg, account, log: warn, info: log });
  // Every agent run in the Gateway, for the status indicator on the devices (none: the host has no event bus).
  let server;
  const tracker = typeof core?.events?.onAgentEvent === 'function'
    ? new ActivityTracker({ onChange: () => server?.refreshActivity() }) : null;
  const sessionKeys = new Map();
  const activity = tracker && {
    summary: (key) => tracker.summary(key),
    sessionKeyFor: (deviceId) => {
      if (!sessionKeys.has(deviceId)) sessionKeys.set(deviceId, deviceRoute({ core, cfg: ctx.cfg, account, deviceId }).route.sessionKey);
      return sessionKeys.get(deviceId);
    },
  };
  let unsubscribe = null, unsubscribeCron = null;
  const cron = deps.cron ?? cronWatcher();
  if (tracker) {
    try { unsubscribe = core.events.onAgentEvent((evt) => tracker.event(evt)); }
    catch (error) { warn(`kubik: agent activity is not shown (${error?.message ?? error})`); }
  }
  server = new KubikServer({
    account, log, setStatus, pairing, activity, cron, agentControl,
    notificationPath: join(deps.lan?.stateDir ?? join(core.state.resolveStateDir(process.env), CHANNEL_ID), "notifications.json"),
    engineFactory: deps.engineFactory ?? createEngine,
    engineOptions: { core, cfg: ctx.cfg },
    ...deps.serverOptions,
    dispatch: (turn) => (deps.dispatch ?? dispatchTranscript)({ ...turn, account, cfg: ctx.cfg, log: warn, info: log, setStatus }),
  });
  try {
    signal?.throwIfAborted();
    server.start();
    servers.set(account.accountId, server);
    const lanError = await startLan({ account, server, core, options: deps.lan, warn });
    unsubscribeCron = cron.subscribe(() => server.refreshCron());
    cron.refresh();
    const { mode, lan, gatewayRoute } = transportStatus(account.accountId);
    log(`kubik: devices connect via ${mode}`);
    setStatus(sdk.channelReadyPatch({ mode, lan, gatewayRoute, lastError: lanError }));
    if (!signal) return server; // tests drive the lifecycle manually
    await new Promise((resolve) => signal.aborted ? resolve() : signal.addEventListener('abort', resolve, { once: true }));
  } catch (error) {
    if (!signal?.aborted) {
      const message = `Kubik device server failed: ${error?.message ?? error}`;
      setStatus({ lastError: message });
      throw new Error(message);
    }
  } finally {
    if (signal) {
      try { unsubscribe?.(); } catch { /* best effort */ }
      unsubscribeCron?.();
      tracker?.close();
      if (servers.get(account.accountId) === server) servers.delete(account.accountId);
      await server.stop();
      setStatus(sdk.channelStoppedPatch({ lastStopAt: Date.now(), onlineDevices: [] }));
    }
  }
  return undefined;
}

/** Adapts OpenClaw's channel pairing store (runtime.channel.pairing) to the server's `pairing` interface. */
export function pairingStore(core, accountId) {
  const api = core?.channel?.pairing;
  if (typeof api?.upsertPairingRequest !== 'function' || typeof api?.readAllowFromStore !== 'function') return null;
  return {
    upsert: (id, meta) => api.upsertPairingRequest({ channel: CHANNEL_ID, id, accountId, meta }),
    allowed: () => api.readAllowFromStore({ channel: CHANNEL_ID, accountId }),
  };
}
