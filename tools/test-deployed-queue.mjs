#!/usr/bin/env node
// Actual host Gateway -> durable queue -> actual device. Explicit screen-only
// tags avoid STT, LLM and TTS requests. Requires an existing USB bridge.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

const args = process.argv.slice(2);
const option = (name, fallback) => args.includes(name) ? args[args.indexOf(name) + 1] : fallback;
const host = option('--host', '');
const controlUrl = option('--control', 'http://127.0.0.1:18791');
const queuePath = option('--queue', '/root/.openclaw/kubik/notifications.json');
const message = '[[show]]Кубик: обновление сервера проверено[[/show]]';
const shellQuote = (value) => `'${String(value).replaceAll("'", "'\\''")}'`;

async function control(body) {
  // SSH below blocks this event loop long enough for HTTP keep-alive to expire.
  // A fresh socket avoids reusing one which the bridge closed in the meantime.
  const response = await fetch(`${controlUrl}/config`, {
    method: 'POST', headers: { 'content-type': 'application/json', connection: 'close' },
    body: JSON.stringify(body), signal: AbortSignal.timeout(5000),
  });
  assert.equal(response.status, 200);
  const data = await response.json();
  assert.equal(data.ok, true);
  return data;
}

async function until(check, label) {
  const deadline = Date.now() + 45000;
  for (let attempt = 0; attempt < 80 && Date.now() < deadline; attempt++) {
    if (await check()) return;
    await sleep(500);
  }
  throw new Error(`Timed out: ${label}`);
}

function remote(command) {
  return execFileSync('ssh', ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', host, command],
    { encoding: 'utf8', timeout: 60000, stdio: ['ignore', 'pipe', 'pipe'] });
}

function checkDiskQueue(phase) {
  const script = [
    'import json,pathlib,sys',
    'p=pathlib.Path(sys.argv[1]);d=json.loads(p.read_text())',
    'ours=[x for x in d["entries"] if x["text"]==sys.argv[2]]',
    'assert (len(ours)==1) if sys.argv[3]=="before" else (len(ours)==0)',
    'assert (p.stat().st_mode & 0o777)==0o600',
  ].join('\n');
  remote(`python3 -c ${shellQuote(script)} ${shellQuote(queuePath)} ${shellQuote(message)} ${shellQuote(phase)}`);
}

async function restore(original) {
  await control({ cmd: 'sim', ev: 'wifi', ms: 1 });
  await control({ cmd: 'card', text: '' });
  await until(async () => {
    const state = await control({ cmd: 'info' });
    return state.online && state.via === 'wifi';
  }, 'original Wi-Fi route restored');
  const current = await control({ cmd: 'info' });
  for (const key of ['ssid', 'url', 'key', 'character', 'volume', 'brightness']) {
    assert.equal(current[key], original[key], `${key} was changed`);
  }
  if (current.screen_dark !== original.screen_dark) await control({ cmd: 'sim', ev: 'pwr' });
  console.log('PASS device settings and paired identity preserved');
}

async function main() {
  if (!/^[a-zA-Z0-9_.-]+@[a-zA-Z0-9_.:-]+$/.test(host)) {
    throw new Error('Usage: node tools/test-deployed-queue.mjs --host user@host [--control URL] [--queue PATH]');
  }
  const original = await control({ cmd: 'info' });
  assert.equal(original.online, true);
  assert.equal(original.via, 'wifi');
  assert.equal(original.menu, false);
  assert.match(original.device, /^[a-z0-9_-]{1,64}$/);
  try {
    if (!original.screen_dark) await control({ cmd: 'sim', ev: 'pwr' });
    await until(async () => (await control({ cmd: 'info' })).screen_dark, 'screen off');
    await control({ cmd: 'sim', ev: 'wifi', ms: 180000 });
    await until(async () => !(await control({ cmd: 'info' })).online, 'device disconnected');
    await sleep(1500);
    const output = remote('export PATH=/root/.npm-global/bin:$PATH; openclaw message send --channel kubik '
      + `--target ${shellQuote(`kubik:${original.device}`)} --message ${shellQuote(message)} --json`);
    const result = JSON.parse(output.slice(output.indexOf('{')));
    assert.equal(result.channel, 'kubik');
    assert.equal(result.dryRun, false);
    checkDiskQueue('before');
    assert.equal((await control({ cmd: 'info' })).screen_dark, true);
    console.log('PASS production notification persisted while device is offline (0600)');
    await control({ cmd: 'sim', ev: 'wifi', ms: 1 });
    await until(async () => {
      const state = await control({ cmd: 'info' });
      return state.online && state.via === 'wifi';
    }, 'authenticated reconnect');
    await until(async () => !(await control({ cmd: 'info' })).screen_dark, 'notification wakes screen');
    checkDiskQueue('after');
    console.log('PASS production notification drained on reconnect and woke the screen; no TTS');
  } finally {
    await restore(original);
  }
}

main().catch((error) => { console.error(error.message); process.exitCode = 1; });
