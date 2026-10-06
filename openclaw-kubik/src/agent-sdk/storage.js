import { closeSync, constants, fstatSync, fsyncSync, mkdirSync, openSync, readSync, renameSync, rmSync, statSync,
  unlinkSync, writeFileSync, chmodSync } from 'node:fs';
import { dirname } from 'node:path';
import { randomBytes } from 'node:crypto';

const LOCK_TIMEOUT_MS = 5000;
const LOCK_STALE_MS = 30_000;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export function ensurePrivateDirectory(path) {
  mkdirSync(path, { recursive: true, mode: 0o700 });
  chmodSync(path, 0o700);
}

export function readJsonFile(path, maxBytes = 64 * 1024) {
  let fd;
  try {
    fd = openSync(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    const stat = fstatSync(fd);
    if (!stat.isFile() || stat.size > maxBytes) throw new Error('state file is invalid or too large');
    const bytes = Buffer.alloc(maxBytes + 1);
    let length = 0;
    while (length < bytes.length) {
      const count = readSync(fd, bytes, length, bytes.length - length, length);
      if (!count) break;
      length += count;
    }
    if (length > maxBytes) throw new Error('state file is too large');
    return JSON.parse(bytes.toString('utf8', 0, length));
  } finally {
    if (fd !== undefined) closeSync(fd);
  }
}

export function writeJsonAtomic(path, value, maxBytes = 64 * 1024) {
  const content = `${JSON.stringify(value)}\n`;
  if (Buffer.byteLength(content) > maxBytes) throw new Error('state file would exceed its size limit');
  const temporary = `${path}.${process.pid}.${randomBytes(6).toString('hex')}.tmp`;
  const fd = openSync(temporary, 'wx', 0o600);
  try {
    writeFileSync(fd, content, { encoding: 'utf8' });
    fsyncSync(fd);
  } catch (error) {
    closeSync(fd);
    rmSync(temporary, { force: true });
    throw error;
  }
  closeSync(fd);
  try {
    renameSync(temporary, path);
    chmodSync(path, 0o600);
    try {
      const directory = openSync(dirname(path), 'r');
      try { fsyncSync(directory); } finally { closeSync(directory); }
    } catch { /* directory fsync is unavailable on some platforms */ }
  } catch (error) {
    rmSync(temporary, { force: true });
    throw error;
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
