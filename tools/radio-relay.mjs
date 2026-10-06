// Optional, explicit SSH relay when the physical device cannot reach the test computer's LAN.
// The remote host forwards encrypted bytes only; no provider calls, Gateway changes or saved files.
import { spawn } from 'node:child_process';
import { randomInt } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { isIP } from 'node:net';
import { setTimeout as sleep } from 'node:timers/promises';

const quote = (value) => `'${value.replaceAll("'", "'\\''")}'`;
export async function startRadioRelay(localHost, localPort) {
  const target = process.env.KUBIK_TEST_RELAY_SSH, publicHost = process.env.KUBIK_TEST_RELAY_IPV4;
  if (!target && !publicHost) return null;
  if (!/^[a-zA-Z0-9][a-zA-Z0-9@._:-]*$/.test(target ?? '') || isIP(publicHost) !== 4) {
    throw Error('Relay requires KUBIK_TEST_RELAY_SSH and KUBIK_TEST_RELAY_IPV4');
  }
  const port = randomInt(40000, 60000);
  const source = readFileSync(new URL('./radio-relay.py', import.meta.url), 'utf8');
  const allowPort = process.env.KUBIK_TEST_RELAY_ALLOW_PORT === '1' ? ' --allow-port' : '';
  const child = spawn('ssh', ['-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', '-o', 'ExitOnForwardFailure=yes',
    '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2', '-R', `127.0.0.1:${port}:${localHost}:${localPort}`,
    target, `python3 -u -c ${quote(source)} ${port} ${quote(publicHost)}${allowPort}`], { stdio: ['pipe', 'pipe', 'pipe'] });
  let exited = false, exitCode, exitSignal, errorOutput = '';
  child.once('exit', (code, signal) => { exited = true; exitCode = code; exitSignal = signal; });
  child.once('error', () => { exited = true; });
  child.stderr.on('data', (data) => { errorOutput = (errorOutput + data).slice(-512); });
  child.stdin.on('error', () => {});
  async function close(requireSuccess = false) {
    child.stdin.end();
    for (let n = 0; n < 50 && !exited; n++) await sleep(100);
    if (!exited) child.kill('SIGTERM');
    for (let n = 0; n < 20 && !exited; n++) await sleep(100);
    if (!exited) child.kill('SIGKILL');
    for (let n = 0; n < 10 && !exited; n++) await sleep(100);
    if (requireSuccess && (!exited || exitCode !== 0)) {
      throw Error(`Radio relay cleanup failed (${exitSignal ?? exitCode ?? 'still running'}): ${errorOutput}`);
    }
  }
  try {
    const remotePort = await new Promise((resolve, reject) => {
      let output = '';
      const timer = setTimeout(() => reject(Error('Radio relay startup timed out')), 15000);
      const finish = (callback, value) => { clearTimeout(timer); callback(value); };
      child.once('error', (err) => finish(reject, err));
      child.once('exit', () => finish(reject, Error(`Radio relay exited: ${errorOutput}`)));
      child.stdout.on('data', (data) => {
        output += data;
        if (output.length > 512) return finish(reject, Error('Invalid radio relay status'));
        if (!output.includes('\n')) return;
        try {
          const { port: value } = JSON.parse(output.trim());
          if (!Number.isInteger(value) || value < 1 || value > 65535) throw Error('Invalid relay port');
          finish(resolve, value);
        } catch (err) { finish(reject, err); }
      });
    });
    return { url: `kubik://${publicHost}:${remotePort}`, close: () => close(true) };
  } catch (error) { await close(); throw error; }
}
