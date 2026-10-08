#!/usr/bin/env node
import { existsSync, realpathSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline/promises';
import { stdin, stdout } from 'node:process';
import { resolveAccount } from '../config.js';
import { importAdapter, normalizeHostConfig, normalizeSetupValues, readHostConfig, validateConfigForAdapter,
  writeHostConfig, VOICE_SETUP_FIELDS } from './index.js';
import { createAgentHost } from './host.js';
import { deviceServerAddresses } from '../lan.js';
import { createFilePairingStore, PAIRING_TTL_MS } from './pairing-store.js';

const DEFAULT_STATE_DIR = join(homedir(), '.config', 'kubik-agent-host');
const DEFAULT_LAN_PORT = 18790;

function expandPath(value) {
  const input = String(value ?? '').trim();
  return resolve(input === '~' ? homedir() : input.startsWith('~/') ? join(homedir(), input.slice(2)) : input);
}

export function shellQuote(value, platform = process.platform) {
  const escaped = String(value).replaceAll("'", platform === 'win32' ? "''" : "'\\''");
  return `'${escaped}'`;
}

export function commandPrefix(script = process.argv[1], executable = process.execPath, platform = process.platform, nodeArgs = process.execArgv) {
  const envFiles = nodeArgs.filter((arg) => arg.startsWith('--env-file=')).map((arg) => shellQuote(arg, platform));
  const command = [shellQuote(executable, platform), ...envFiles, shellQuote(resolve(script), platform)].join(' ');
  return platform === 'win32' ? `& ${command}` : command;
}

export function parseArgs(argv) {
  let stateDir = process.env.KUBIK_AGENT_STATE_DIR || DEFAULT_STATE_DIR;
  let command = '';
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--state-dir') {
      if (!argv[i + 1]) throw new Error('--state-dir needs a path');
      stateDir = argv[++i];
    } else if (argv[i] === '--help' || argv[i] === '-h') command = 'help';
    else if (!command) command = argv[i];
    else positional.push(argv[i]);
  }
  return { command: command || 'help', positional, stateDir: expandPath(stateDir) };
}

export async function collectSetupValues(fields, ask, existing = {}) {
  const input = {};
  for (const field of fields) {
    const previous = Object.hasOwn(existing, field.key) ? existing[field.key] : field.default;
    const fallback = previous === undefined ? '' : String(previous);
    let answer;
    if (field.type === 'boolean') {
      const yesDefault = previous === true;
      answer = (await ask(`${field.label} ${yesDefault ? '[Y/n]' : '[y/N]'}: `)).trim().toLowerCase();
      input[field.key] = answer === '' ? Boolean(previous) : ['y', 'yes', 'да', 'д'].includes(answer);
      if (answer && !['y', 'yes', 'да', 'д', 'n', 'no', 'нет', 'н'].includes(answer)) {
        throw new Error(`enter yes or no for setup field "${field.key}"`);
      }
      if (answer && ['n', 'no', 'нет', 'н'].includes(answer)) input[field.key] = false;
      continue;
    }
    answer = (await ask(`${field.label}${fallback ? ` [${fallback}]` : ''}: `)).trim();
    const value = answer || (previous === undefined ? undefined : previous);
    if (value === undefined) continue;
    if (field.type === 'integer' && typeof value === 'string') {
      if (!/^-?\d{1,10}$/.test(value)) throw new Error(`setup field "${field.key}" must be an integer`);
      input[field.key] = Number(value);
    } else input[field.key] = value;
  }
  return normalizeSetupValues(fields, input);
}

function help() {
  const command = commandPrefix();
  return [
    'Kubik agent host',
    '',
    `  ${command} [--state-dir PATH] setup`,
    `  ${command} [--state-dir PATH] serve`,
    `  ${command} [--state-dir PATH] pair-list`,
    `  ${command} [--state-dir PATH] approve CODE`,
  ].join('\n');
}

function configuredAdapter(config) {
  if (config.adapter.modulePath) return importAdapter(config.adapter.modulePath);
  throw new Error(`adapter "${config.adapter.id}" needs an absolute modulePath in agent-host.json`);
}

async function withPrompt(callback) {
  if (!stdin.isTTY || !stdout.isTTY) throw new Error('setup requires an interactive terminal');
  const terminal = createInterface({ input: stdin, output: stdout });
  try { return await callback((prompt) => terminal.question(prompt)); }
  finally { terminal.close(); }
}

function resolveVoice(voice, env) {
  const account = resolveAccount({ channels: { kubik: {
    enabled: true, voice: { provider: 'openai-http', ...voice },
  } } }, 'default', { env, readSecrets: true });
  if (!account.voice.apiKey) throw new Error('Set OPENAI_API_KEY before setting up the voice host');
  return account;
}

async function selectAdapter(ask, previous) {
  const oldPath = previous?.adapter.modulePath ?? '';
  const enteredPath = (await ask(`Path to the .mjs/.js adapter module${oldPath ? ` [${oldPath}]` : ''}: `)).trim() || oldPath;
  if (!enteredPath) throw new Error('custom adapter module path is required');
  const modulePath = expandPath(enteredPath);
  return { adapter: await importAdapter(modulePath), modulePath };
}

async function collectHostSettings(ask, previous) {
  const oldPort = previous?.listener.port ?? DEFAULT_LAN_PORT;
  const enteredPort = (await ask(`Kubik TCP port [${oldPort}]: `)).trim();
  const port = enteredPort ? Number(enteredPort) : oldPort;
  const oldHost = previous?.listener.host ?? '';
  const listenerHost = (await ask(`Bind address (blank for all interfaces${oldHost ? `; current: ${oldHost}` : ''}): `)).trim();
  const voice = await collectSetupValues(VOICE_SETUP_FIELDS, ask, previous?.voice);
  return { port, listenerHost, voice };
}

async function probeAdapter(adapter, config, env) {
  let attempted = false;
  try {
    attempted = true;
    const result = await adapter.connect({ config: config.adapter.setup, env });
    stdout.write(`Connected to ${adapter.label}${result?.model ? ` (model ${result.model})` : ''}.\n`);
  } finally { if (attempted) await adapter.close?.(); }
}

function printSetupInstructions(stateDir, port, config, adapter) {
  const command = `${commandPrefix()} --state-dir ${shellQuote(stateDir)}`;
  stdout.write(`Configuration saved to ${join(stateDir, 'agent-host.json')}.\n`);
  stdout.write('\nNext:\n');
  stdout.write(`  ${command} serve\n`);
  stdout.write('  Connect Kubik to the same trusted home network. Leave the server address blank to discover this host over UDP 18790.\n');
  stdout.write(`  If discovery is blocked, use kubik://<host-address>:${port}; allow TCP ${port} and UDP 18790 on your local network.\n`);
  stdout.write(`  When Kubik shows a code, run ${command} pair-list. Only approve a matching code with ${command} approve CODE.\n`);
  const envNames = adapter.setup.filter((field) => field.type === 'env')
    .map((field) => config.adapter.setup[field.key]).filter(Boolean);
  stdout.write(`  Keep OPENAI_API_KEY set for voice; the adapter uses ${[...new Set(envNames)].join(', ') || 'its configured environment variables'}. Keep these variables available when starting the host; values are not saved in its configuration.\n`);
}

export async function runSetupWizard(ask, stateDir, env, previous) {
  const { adapter, modulePath } = await selectAdapter(ask, previous);
  const defaults = previous?.adapter.id === adapter.id ? previous.adapter.setup : {};
  const setup = await collectSetupValues(adapter.setup, ask, defaults);
  const { port, listenerHost, voice } = await collectHostSettings(ask, previous);
  resolveVoice(voice, env);
  const config = normalizeHostConfig({ version: 2, listener: { port, ...(listenerHost ? { host: listenerHost } : {}) }, voice,
    adapter: { id: adapter.id, ...(modulePath ? { modulePath } : {}), setup } });
  await probeAdapter(adapter, validateConfigForAdapter(config, adapter), env);
  await writeHostConfig(stateDir, config);
  printSetupInstructions(stateDir, port, config, adapter);
}

async function setup(stateDir, env = process.env) {
  const previous = existsSync(join(stateDir, 'agent-host.json')) ? await readHostConfig(stateDir) : null;
  await withPrompt((ask) => runSetupWizard(ask, stateDir, env, previous));
}

async function serve(stateDir, env = process.env) {
  const config = await readHostConfig(stateDir);
  const adapter = await configuredAdapter(config);
  const host = await createAgentHost({ stateDir, config, adapter, env, serverOptions: { notificationPath: join(stateDir, 'notifications.json') },
    log: (message) => process.stderr.write(`${message}\n`) });
  const status = host.listener.status;
  const addresses = deviceServerAddresses(status);
  stdout.write('Same home network: leave Server address blank. No domain is needed.\n');
  for (const address of addresses) stdout.write(`Server address: ${address}\n`);
  if (!addresses.length) stdout.write('No reachable LAN IPv4 address. Check the network and listener bind address.\n');
  stdout.write('Use the address on the same network as Kubik. Keep this host running.\n');
  stdout.write('Pair devices: pair-list, then approve CODE. Stop: Ctrl+C.\n');
  await new Promise((resolveStop) => {
    let stopping = false;
    const stop = async () => {
      if (stopping) return;
      stopping = true;
      process.off('SIGINT', stopSignal);
      process.off('SIGTERM', stopSignal);
      try { await host.close(); }
      catch (error) { process.stderr.write(`Shutdown failed: ${error?.message ?? error}\n`); process.exitCode = 1; }
      resolveStop();
    };
    const stopSignal = () => { void stop(); };
    process.once('SIGINT', stopSignal);
    process.once('SIGTERM', stopSignal);
  });
}

function pairingTimeLeft(createdAt, listedAt) {
  const seconds = Math.ceil(Math.max(0, PAIRING_TTL_MS - (listedAt - createdAt)) / 1000);
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return `${hours}h ${minutes}m ${seconds % 60}s`;
}

async function pairingList(stateDir) {
  const pairing = await createFilePairingStore(stateDir).ready();
  const listedAt = Date.now();
  const state = await pairing.list();
  if (!state.pending.length && !state.approved.length) { stdout.write('No pending or approved devices.\n'); return; }
  for (const item of state.pending) {
    stdout.write(`Pending: ${item.entry} (${item.name || 'unnamed'}), code ${item.code}, expires in ${pairingTimeLeft(item.createdAt, listedAt)}\n`);
  }
  for (const item of state.approved) stdout.write(`Approved: ${item}\n`);
}

async function approvePairing(stateDir, code) {
  if (!code) throw new Error('provide the CODE shown on Kubik');
  const pairing = await createFilePairingStore(stateDir).ready();
  const approved = await pairing.approve(code);
  stdout.write(`Approved: ${approved.entry}. The connected Kubik will gain access within a few seconds.\n`);
}

export async function runCli(argv = process.argv.slice(2), env = process.env) {
  const parsed = parseArgs(argv);
  if (parsed.command === 'help') { stdout.write(`${help()}\n`); return; }
  if (parsed.command === 'setup') return setup(parsed.stateDir, env);
  if (parsed.command === 'serve') return serve(parsed.stateDir, env);
  if (parsed.command === 'pair-list') return pairingList(parsed.stateDir);
  if (parsed.command === 'approve') return approvePairing(parsed.stateDir, parsed.positional[0]);
  throw new Error(`unknown command "${parsed.command}"; run with --help`);
}

if (process.argv[1] && realpathSync(resolve(process.argv[1])) === realpathSync(new URL(import.meta.url))) {
  runCli().catch((error) => {
    process.stderr.write(`Error: ${error?.message ?? error}\n`);
    process.exitCode = 1;
  });
}
