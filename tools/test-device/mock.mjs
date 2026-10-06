// Temporary USB mock. Startup failures and normal completion both restore the observed primary route.
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

async function until(check, label, timeoutMs, intervalMs) {
  const deadline = Date.now() + timeoutMs;
  do {
    if (await check()) return;
    await sleep(intervalMs);
  } while (Date.now() < deadline);
  throw Error(`Timed out: ${label}`);
}

async function closeChild(child, closed, timeoutMs, intervalMs) {
  if (closed()) return;
  child.kill('SIGTERM');
  try { await until(closed, 'mock process exit', timeoutMs, intervalMs); }
  catch { child.kill('SIGKILL'); await until(closed, 'forced mock exit', timeoutMs, intervalMs); }
}

export async function startMock({ device, sim, port, holdMs, spawnChild = spawn,
  startupMs = 5000, connectMs = 40000, restoreMs = 45000, closeMs = 5000, intervalMs = 100 }) {
  const original = await device({ cmd: 'info' });
  const child = spawnChild('node', [new URL('../usb-bridge/parrot-server.mjs', import.meta.url).pathname,
    String(port), '1800'], { stdio: ['ignore', 'pipe', 'inherit'] });
  let output = '', failed, closed = false, paused = false, stopped = false;
  child.stdout.on('data', (data) => { output = (output + data).slice(-8192); });
  child.on('error', (error) => { failed = error; });
  child.on('close', () => { closed = true; });
  const stop = async () => {
    if (stopped) return;
    stopped = true;
    try { if (paused) await sim('wifi', { ms: 1 }); }
    finally { await closeChild(child, () => closed, closeMs, intervalMs); }
    if (!paused) return;
    await until(async () => {
      const state = await device({ cmd: 'info' });
      return state.via === original.via && state.wifi_connected === original.wifi_connected;
    }, 'original route after mock', restoreMs, intervalMs);
  };
  try {
    await until(() => {
      if (failed) throw failed;
      if (closed) throw Error('Mock exited before startup');
      return /parrot on/.test(output);
    }, 'mock startup', startupMs, intervalMs);
    paused = true; // even a lost config reply may have applied the pause
    await sim('wifi', { ms: holdMs });
    await until(async () => {
      if (failed) throw failed;
      if (closed) throw Error('Mock exited before device connection');
      return (await device({ cmd: 'info' })).via === 'usb' && /device connected/.test(output);
    }, `USB mock connection; point bridge at ws://127.0.0.1:${port}/kubik/v1`, connectMs, intervalMs);
    return { stop };
  } catch (error) {
    try { await stop(); }
    catch (cleanup) { throw new AggregateError([error, cleanup], 'Mock startup and restoration failed'); }
    throw error;
  }
}
