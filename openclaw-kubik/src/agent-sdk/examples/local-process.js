// Copy this file and point command at an executable that accepts --session ID.
// It reads a transcript on stdin and writes one UTF-8 reply to stdout; no shell.
import { access } from 'node:fs/promises';
import { constants } from 'node:fs';
import { isAbsolute } from 'node:path';
import { spawn } from 'node:child_process';
import { defineAdapter } from 'openclaw-kubik/agent-sdk';

const MAX_CHILDREN = 8;
const MAX_OUTPUT_BYTES = 65_536;
const MAX_REPLY_CHARS = 8192;
const REQUEST_TIMEOUT_MS = 30_000;
let command = '';
const pending = new Set();

function ask(deviceId, transcript, signal) {
  signal.throwIfAborted();
  const child = spawn(command, ['--session', deviceId], {
    stdio: ['pipe', 'pipe', 'ignore'],
  });
  const chunks = [];
  let size = 0, failed = false;
  const stop = () => {
    failed = true;
    child.kill('SIGKILL');
    child.stdin.destroy();
    child.stdout.destroy();
  };
  const abort = () => stop();
  const timer = setTimeout(stop, REQUEST_TIMEOUT_MS);
  signal.addEventListener('abort', abort, { once: true });
  child.on('error', () => { failed = true; });
  child.stdin.on('error', stop);
  child.stdout.on('error', stop);
  child.stdout.on('data', chunk => {
    size += chunk.length;
    if (size > MAX_OUTPUT_BYTES) stop();
    else if (!failed) chunks.push(chunk);
  });
  return new Promise((resolve, reject) => {
    child.once('close', code => {
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
      if (signal.aborted) { resolve(); return; }
      if (failed || code !== 0) { reject(new Error('Agent process failed')); return; }
      const text = Buffer.concat(chunks, size).toString('utf8').trim();
      if (!text || text.length > MAX_REPLY_CHARS) { reject(new Error('Invalid reply')); return; }
      resolve(text);
    });
    child.stdin.end(transcript);
  });
}

export default defineAdapter({
  id: 'local-process', label: 'Local process example',
  setup: [{ key: 'command', label: 'Agent executable path', type: 'string', required: true }],
  async connect({ config }) {
    if (!isAbsolute(config.command)) throw new Error('Use an absolute executable path');
    try { await access(config.command, constants.X_OK); }
    catch { throw new Error('Agent executable is unavailable'); }
    command = config.command;
  },
  async reply({ device, transcript, signal }) {
    if (pending.size >= MAX_CHILDREN) throw new Error('Agent is busy');
    const marker = {};
    pending.add(marker);
    try { return await ask(device.id, transcript, signal); }
    finally { pending.delete(marker); }
  },
  close() { command = ''; },
});
