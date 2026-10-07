import { closeSync, fstatSync, fsyncSync, openSync, renameSync, rmSync, statSync, unlinkSync, writeFileSync } from 'node:fs';
import { randomBytes } from 'node:crypto';
import { ensurePrivateDirectory as ensurePrivateStateDirectory, readPrivateFile, writePrivateFileAtomic } from '../private-storage.js';

const LOCK_TIMEOUT_MS = 5000;
const LOCK_STALE_MS = 30_000;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export function ensurePrivateDirectory(path) {
  ensurePrivateStateDirectory(path, 'agent SDK state directory');
}

export function readJsonFile(path, maxBytes = 64 * 1024) {
  const file = readPrivateFile(path, maxBytes, 'agent SDK state file');
  if (file.text === null) {
    const error = new Error('ENOENT: agent SDK state file not found');
    error.code = 'ENOENT';
    error.path = path;
    error.syscall = 'open';
    throw error;
  }
  if (file.tooLarge) throw new Error('agent SDK state file is too large');
  try { return JSON.parse(file.text); }
  catch { throw new Error('agent SDK state file contains invalid JSON'); }
}

export function writeJsonAtomic(path, value, maxBytes = 64 * 1024) {
  const content = `${JSON.stringify(value)}\n`;
  if (Buffer.byteLength(content) > maxBytes) throw new Error('agent SDK state file would exceed its size limit');
  const syncError = writePrivateFileAtomic(path, content);
  if (syncError) {
    try {
      process.emitWarning('agent SDK state was committed but directory sync failed', {
        code: 'KUBIK_AGENT_STATE_DIRSYNC',
      });
    } catch { /* a warning handler cannot undo the rename */ }
  }
}

/** A short cross-process lock for the host configuration and pairing file. */
export async function withFileLock(path, callback, { timeoutMs = LOCK_TIMEOUT_MS } = {}) {
  const lockPath = `${path}.lock`;
  const deadline = Date.now() + timeoutMs;
  let fd;
  while (fd === undefined) {
    try {
      fd = openSync(lockPath, 'wx', 0o600);
      writeFileSync(fd, `${process.pid} ${Date.now()}\n`, 'utf8');
      fsyncSync(fd);
    } catch (error) {
      if (fd !== undefined) { closeSync(fd); fd = undefined; rmSync(lockPath, { force: true }); }
      if (error.code !== 'EEXIST') throw error;
      try {
        if (Date.now() - statSync(lockPath).mtimeMs > LOCK_STALE_MS) {
          const stale = `${lockPath}.stale-${randomBytes(4).toString('hex')}`;
          try { renameSync(lockPath, stale); rmSync(stale, { force: true }); } catch { /* another process won */ }
          continue;
        }
      } catch { /* the owner released the lock between open and stat */ }
      if (Date.now() >= deadline) throw new Error('timed out waiting for the host state lock');
      await sleep(25);
    }
  }
  const owned = fstatSync(fd);
  try { return await callback(); }
  finally {
    closeSync(fd);
    try {
      const current = statSync(lockPath);
      if (current.dev === owned.dev && current.ino === owned.ino) unlinkSync(lockPath);
    } catch { /* a stale-lock recovery already removed it */ }
  }
}
