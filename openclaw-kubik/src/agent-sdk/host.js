import { resolveAccount } from '../config.js';
import { HttpEngine } from '../engines/http.js';
import { LanListener } from '../lan.js';
import { KubikServer } from '../server.js';
import { loadTlsIdentity } from '../tls-identity.js';
import { join } from 'node:path';
import { normalizeHostConfig, validateConfigForAdapter } from './config.js';
import { createActivityRelay, createCronRelay, MAX_EVENT_TTL_MS } from './events.js';
import { createFilePairingStore } from './pairing-store.js';
import { ensurePrivateDirectory } from './storage.js';
import { createAdapterControl } from './controls.js';

const createHttpEngine = (voice, options) => new HttpEngine(voice, options);

/** Runs the existing Kubik protocol server outside OpenClaw with a local, explicitly approved pairing store. */
export async function createAgentHost({ stateDir, config, adapter, env = process.env, lanOptions = {},
  engineFactory = createHttpEngine, engineOptions = {}, serverOptions = {}, log = () => {} } = {}) {
  if (typeof stateDir !== 'string' || !stateDir.trim()) throw new Error('agent host state directory is required');
  ensurePrivateDirectory(stateDir);
  const stored = normalizeHostConfig(config);
  const ready = validateConfigForAdapter(stored, adapter);
  const accountConfig = { channels: { kubik: {
    enabled: true,
    listen: { enabled: true, port: ready.listener.port },
    voice: { provider: 'openai-http', ...ready.voice },
  } } };
  const account = resolveAccount(accountConfig, 'default', { env, readSecrets: true });
  if (!account.voice.apiKey) throw new Error('Set OPENAI_API_KEY before starting the voice host');

  const pairing = await createFilePairingStore(stateDir).ready();
  let adapterAttempted = false;
  let server;
  let listener;
  let activity;
  let cron;
  let closed = false;
  let closePromise;
  const requireOpen = () => { if (closed) throw new Error('agent host is closed'); };
  try {
    adapterAttempted = true;
    await adapter.connect({ config: ready.adapter.setup, env, log });
    activity = createActivityRelay({ onChange: () => server?.refreshActivity() });
    cron = createCronRelay({ onChange: () => server?.refreshCron() });
    const events = Object.freeze({
      activity: (deviceId, snapshot, ttlMs) => { requireOpen(); return activity.set(deviceId, snapshot, ttlMs); },
      cron: (snapshot, ttlMs = MAX_EVENT_TTL_MS) => { requireOpen(); return cron.set(snapshot, ttlMs); },
      notify: (deviceId, text, options) => {
        requireOpen();
        if (typeof deviceId !== 'string' || deviceId.length > 64 || typeof text !== 'string' || !text.trim() || Buffer.byteLength(text) > 4096) {
          throw new Error('notification needs a device id and text of at most 4096 bytes');
        }
        if (typeof server.notify !== 'function') throw new Error('KubikServer.notify(deviceId, text) is unavailable');
        return server.notify(deviceId, text, options);
      },
    });
    server = new KubikServer({
      ...serverOptions,
      account,
      pairing,
      activity,
      cron,
      notificationPath: serverOptions.notificationPath ?? join(stateDir, 'notifications.json'),
      engineFactory,
      engineOptions,
      agentControl: createAdapterControl(adapter, account.voice),
      log,
      dispatch: (turn) => adapter.dispatch({ ...turn, events }),
    });
    server.start();
    listener = new LanListener({
      ...lanOptions,
      identity: loadTlsIdentity(stateDir, { log }),
      port: ready.listener.port,
      ...(ready.listener.host ? { host: ready.listener.host } : {}),
      onUpgrade: (req, socket, head, transport) => server.handleUpgrade(req, socket, head, transport),
      log,
    });
    await listener.start();
    server.lan = { listener };
    await adapter.start?.({ events, log });
    return {
      account, config: ready, adapter, pairing, server, listener, events,
      close() {
        if (closePromise) return closePromise;
        closed = true;
        closePromise = (async () => {
          try { await server.stop(); }
          finally {
            activity?.close();
            cron?.close();
            if (adapterAttempted) {
              adapterAttempted = false;
              await adapter.close?.();
            }
          }
        })();
        return closePromise;
      },
      get closed() { return closed; },
    };
  } catch (error) {
    closed = true;
    try { await server?.stop(); } catch { /* preserve the startup failure */ }
    try { await listener?.stop(); } catch { /* preserve the startup failure */ }
    try { activity?.close(); } catch { /* preserve the startup failure */ }
    try { cron?.close(); } catch { /* preserve the startup failure */ }
    try { if (adapterAttempted) await adapter.close?.(); } catch { /* preserve the startup failure */ }
    throw error;
  }
}
