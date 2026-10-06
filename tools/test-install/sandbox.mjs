// Isolated OpenClaw for the install test: temp HOME/state/config, a non-default Gateway port, a private npm prefix
// with the OpenClaw version the plugin declares, and helpers to run documented commands and the Gateway.
import { execFile, spawn } from 'node:child_process';
import { createSocket } from 'node:dgram';
import { createServer } from 'node:net';
import { openSync } from 'node:fs';
import { mkdir, readFile, stat } from 'node:fs/promises';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const KEPT_ENV = ['PATH', 'LANG', 'LC_ALL', 'TERM', 'TMPDIR', 'SystemRoot'];

/** A free TCP port on loopback. */
export function freePort() {
  return new Promise((resolve, reject) => {
    const server = createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => { const { port } = server.address(); server.close(() => resolve(port)); });
  });
}

/** True when TCP `port` can be bound on all IPv4 interfaces (the LAN listener default is 18790). */
export function tcpFree(port) {
  return new Promise((resolve) => {
    const server = createServer();
    server.once('error', () => resolve(false));
    server.listen(port, '0.0.0.0', () => server.close(() => resolve(true)));
  });
}

/** Rejects unless UDP `port` can be bound on all interfaces (the plugin's discovery socket is fixed at 18790). */
export function assertUdpFree(port) {
  return new Promise((resolve, reject) => {
    const socket = createSocket('udp4');
    socket.once('error', (error) => reject(new Error(`UDP ${port} is busy (${error.code}); stop the other Kubik gateway (tools/dev-down.sh)`)));
    socket.bind(port, () => socket.close(() => resolve()));
  });
}

/** Lowest supported OpenClaw version of a range such as ">=2026.9.3 <2027", checked against `version`. */
export function satisfiesRange(range, version) {
  const parts = (text) => text.split('.').map(Number);
  const [, low, high] = /^>=\s*([\d.]+)\s*<\s*([\d.]+)$/.exec(range) ?? [];
  if (!low) throw new Error(`unsupported peer range ${range}`);
  const compare = (a, b) => { for (let i = 0; i < 3; i++) if ((a[i] ?? 0) !== (b[i] ?? 0)) return (a[i] ?? 0) - (b[i] ?? 0); return 0; };
  return compare(parts(version), parts(low)) >= 0 && compare(parts(version), parts(high)) < 0;
}

/** Real ~/.openclaw fingerprint, to prove the run did not touch it. */
export async function realStateFingerprint() {
  const files = ['.openclaw/openclaw.json', '.openclaw/kubik/lan-tls.json'].map((file) => join(homedir(), file));
  return Promise.all(files.map((file) => stat(file).then((info) => `${file}:${info.mtimeMs}`, () => `${file}:absent`)));
}

export class Sandbox {
  /** `dir` is the temp root; `gatewayPort` a non-default port. */
  constructor(dir, gatewayPort) {
    this.dir = dir;
    this.gatewayPort = gatewayPort;
    this.prefix = join(dir, 'openclaw-host');
    this.home = join(dir, 'home');
    this.stateDir = join(this.home, 'state');
    this.configPath = join(this.home, 'openclaw.json');
    this.gatewayLog = join(dir, 'gateway.log');
    this.gateway = null;
    this.env = {
      ...Object.fromEntries(KEPT_ENV.filter((name) => process.env[name]).map((name) => [name, process.env[name]])),
      HOME: this.home, OPENCLAW_HOME: this.home, OPENCLAW_STATE_DIR: this.stateDir, OPENCLAW_CONFIG_PATH: this.configPath,
      OPENCLAW_GATEWAY_PORT: String(gatewayPort), NO_COLOR: '1',
    };
    this.env.PATH = `${join(this.prefix, 'node_modules/.bin')}:${process.env.PATH}`;
  }

  /** Installs the OpenClaw npm package into the private prefix; returns its version. */
  async installOpenClaw(version) {
    await mkdir(this.home, { recursive: true });
    await execFileAsync('npm', ['install', '--prefix', this.prefix, '--no-audit', '--no-fund', '--loglevel=error', `openclaw@${version}`],
      { env: this.env, maxBuffer: 16_777_216 });
    return JSON.parse(await readFile(join(this.prefix, 'node_modules/openclaw/package.json'), 'utf8')).version;
  }

  /** Runs one command line through `sh`, exactly as a person would type it; resolves `{ ok, output }` (never rejects). */
  async run(commandLine, { timeoutMs = 120_000 } = {}) {
    try {
      const { stdout, stderr } = await execFileAsync('sh', ['-c', commandLine], { env: this.env, cwd: this.dir, timeout: timeoutMs, maxBuffer: 16_777_216 });
      return { ok: true, output: stdout + stderr };
    } catch (error) {
      return { ok: false, output: `${error.stdout ?? ''}${error.stderr ?? ''}${error.message}` };
    }
  }

  /** Like `run`, but throws with the command output when it fails. */
  async mustRun(commandLine, options) {
    const result = await this.run(commandLine, options);
    if (!result.ok) throw new Error(`command failed: ${commandLine}\n${result.output.slice(-1500)}`);
    return result.output;
  }

  /** Starts `openclaw gateway run` (the documented foreground start) and waits until `channels status --probe` matches `ready` (default: the Kubik channel runs). */
  async startGateway(ready = /Kubik default:.*running/) {
    const log = openSync(this.gatewayLog, 'a');
    this.gateway = spawn('openclaw', ['gateway', 'run'], { env: this.env, cwd: this.dir, detached: true, stdio: ['ignore', log, log] });
    const exited = new Promise((resolve) => this.gateway.once('exit', (code) => resolve(code)));
    for (let attempt = 0; attempt < 120; attempt++) {
      if (await Promise.race([exited, new Promise((r) => setTimeout(() => r(null), 500))]) !== null) throw new Error('gateway exited early');
      const { output } = await this.run('openclaw channels status --probe');
      if (ready.test(output)) return output;
    }
    throw new Error(`gateway did not match ${ready} in 60 s`);
  }

  /** SIGTERM to the Gateway process group, SIGKILL after 10 s. */
  async stopGateway() {
    const child = this.gateway;
    this.gateway = null;
    if (!child || child.exitCode !== null) return;
    const done = new Promise((resolve) => child.once('exit', resolve));
    try { process.kill(-child.pid, 'SIGTERM'); } catch { /* already gone */ }
    const timer = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch { /* gone */ } }, 10_000);
    await done;
    clearTimeout(timer);
  }
}
