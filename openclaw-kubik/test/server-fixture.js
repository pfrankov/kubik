import { mkdtempSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { registerDeviceRoute } from '../src/gateway-route.js';
import { getServer, monitorAccount } from '../src/monitor.js';
import { account, config, DEFAULT_DEVICE_KEY, fakeEngine, fakePairing, installRuntime, tone } from './helpers.js';

/** A temporary plugin state dir, removed after the test. */
export function stateDir(t) {
  const dir = mkdtempSync(join(tmpdir(), 'kubik-state-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
}

/**
 * A stand-in for the OpenClaw Gateway: a real HTTP server whose upgrades go through the route the plugin
 * registers with `api.registerHttpRoute` (like the Gateway's plugin route dispatch). Its client address is the
 * `X-Forwarded-For` header when present, as the Gateway resolves it behind `gateway.trustedProxies`. `trust` is
 * what the route gets to recognise a trusted proxy peer (`{ proxies(), isTrusted(ip, proxies) }`); none = no proxy is trusted.
 */
export async function fakeGateway(t, serverFor = () => getServer(), trust) {
  const routes = [];
  let clientIp;
  registerDeviceRoute({ registerHttpRoute: (route) => routes.push(route) }, serverFor, () => ({ client: { clientIp } }), trust);
  const [route] = routes;
  const http = createServer(async (req, res) => {
    if (new URL(req.url, 'http://gw').pathname === route.path && await route.handler(req, res)) return;
    res.statusCode = 404; res.end();
  });
  http.on('upgrade', async (req, socket, head) => {
    clientIp = req.headers['x-forwarded-for'];
    if (new URL(req.url, 'http://gw').pathname === route.path && await route.handleUpgrade(req, socket, head)) return;
    socket.end('HTTP/1.1 404 Not Found\r\n\r\n');
  });
  await new Promise((resolve) => http.listen(0, '127.0.0.1', resolve));
  const sockets = new Set();
  http.on('connection', (socket) => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
  t.after(() => { http.close(); for (const socket of sockets) socket.destroy(); });
  return { route, http, url: `ws://127.0.0.1:${http.address().port}/kubik/v1` };
}

/**
 * Runs the channel like the Gateway does: server + LAN TLS listener (loopback, ephemeral ports, temp state dir)
 * + the Gateway route on a fake Gateway. `url` is the Gateway route (devices sign `ca:127.0.0.1`), `lanUrl` the LAN listener.
 */
export async function start(t, { engine = fakeEngine(), runtime = {}, settings = {}, engineFactory, pairing, serverOptions = {}, cron,
  lan = {}, lanPort = 0, listen = {}, trust } = {}) {
  const activePairing = pairing === undefined ? fakePairing({ allowed: [DEFAULT_DEVICE_KEY.entry] }) : pairing;
  const directory = lan.stateDir ?? stateDir(t);
  const { seen, sdk } = installRuntime({ ...runtime, pairing: activePairing?.api,
    extraCore: { ...runtime.extraCore, state: { resolveStateDir: () => directory } } });
  const logs = [];
  const statuses = [];
  const acc = account(settings);
  acc.listen = { port: lanPort, ...listen };
  const gateway = await fakeGateway(t, undefined, trust);
  const server = await monitorAccount({ account: acc, cfg: config(settings), setStatus: (p) => statuses.push(p),
    log: { info: (m) => logs.push(m), warn: (m) => logs.push(m) } },
  { sdk, cron, serverOptions: { engineFactory: engineFactory ?? (() => engine), ...serverOptions },
    lan: { stateDir: directory, host: '127.0.0.1', discoveryHost: '127.0.0.1', discoveryPort: 0, ...lan } });
  t.after(() => { globalThis[Symbol.for('openclaw.kubik.servers')].delete(acc.accountId); return server.stop(); });
  const ready = statuses.findLast((status) => status.lan);
  return { server, seen, logs, statuses, engine, gateway, url: gateway.url, lan: ready?.lan,
    lanUrl: ready?.lan?.port ? `wss://127.0.0.1:${ready.lan.port}/kubik/v1` : undefined };
}

export function talk(device, turn, ms = 600) {
  device.send({ t: 'ptt', on: true, turn });
  device.sendAudio(turn, tone(ms));
  device.send({ t: 'ptt', on: false, turn, ms });
}

export const types = (device) => device.events.map((event) => event.t === 'state' ? `state:${event.s}`
  : event.t === 'emotion' ? `emotion:${event.e}` : event.t === 'error' ? `error:${event.code}` : event.t);
