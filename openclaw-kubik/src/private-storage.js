import { randomUUID } from 'node:crypto';
import { chmodSync, closeSync, constants, fchmodSync, fstatSync, fsyncSync, lstatSync, mkdirSync, openSync,
  readSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';

const UNSUPPORTED_DIRECTORY_SYNC = new Set(['EINVAL', 'ENOTSUP', 'EOPNOTSUPP', 'ENOSYS']);

/** Creates or repairs a private state directory without chmod-following a symlink on Unix. */
export function ensurePrivateDirectory(dir, label = 'private state') {
  mkdirSync(dir, { recursive: true, mode: 0o700 });
  // Windows does not support O_DIRECTORY / O_NOFOLLOW and opening a directory descriptor is not portable there.
  // lstat keeps the portable fallback from following an existing symlink. Windows chmod only controls writability.
  if (process.platform === 'win32') {
    const stat = lstatSync(dir);
    if (stat.isSymbolicLink() || !stat.isDirectory()) throw new Error(`${label} path is not a directory`);
    chmodSync(dir, 0o700);
    return;
  }
  const flags = constants.O_RDONLY | (constants.O_DIRECTORY ?? 0) | (constants.O_NOFOLLOW ?? 0);
  const fd = openSync(dir, flags);
  try {
    const stat = fstatSync(fd);
    if (!stat.isDirectory()) throw new Error(`${label} path is not a directory`);
    if ((stat.mode & 0o7777) !== 0o700) fchmodSync(fd, 0o700);
  } finally { closeSync(fd); }
}

/** Opens without following symlinks where supported, repairs mode, and reads at most maxBytes plus one byte. */
export function readPrivateFile(path, maxBytes, label = 'private state file') {
  let fd;
  try {
    // O_NOFOLLOW is not available on Windows. Reject an already-present link there before opening it; Unix uses
    // the descriptor flag for the race-resistant check.
    if (process.platform === 'win32' && lstatSync(path).isSymbolicLink()) throw new Error(`${label} is not a regular file`);
    fd = openSync(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0) | (constants.O_NONBLOCK ?? 0));
  } catch (error) {
    if (error.code === 'ENOENT') return { text: null, tooLarge: false };
    throw error;
  }
  try {
    const stat = fstatSync(fd);
    if (!stat.isFile()) throw new Error(`${label} is not a regular file`);
    if ((stat.mode & 0o7777) !== 0o600) fchmodSync(fd, 0o600);
    if (stat.size > maxBytes) return { text: '', tooLarge: true };
    const bytes = readBounded(fd, maxBytes + 1);
    if (bytes.length > maxBytes) return { text: '', tooLarge: true };
    return { text: bytes.toString('utf8'), tooLarge: false };
  } finally { closeSync(fd); }
}

/** Replaces a private file. A post-rename directory sync error is returned because rename already committed it. */
export function writePrivateFileAtomic(path, text) {
  const temporary = `${path}.${randomUUID()}.tmp`;
  const fd = openSync(temporary, 'wx', 0o600);
  let committed = false;
  try {
    try {
      writeFileSync(fd, text);
      fsyncSync(fd);
    } finally { closeSync(fd); }
    renameSync(temporary, path);
    committed = true;
  } finally { if (!committed) rmSync(temporary, { force: true }); }
  return syncDirectory(dirname(path));
}

function syncDirectory(path) {
  if (process.platform === 'win32') return null;
  let fd, syncFailure, closeFailure;
  try { fd = openSync(path, 'r'); fsyncSync(fd); }
  catch (error) { syncFailure = error; }
  if (fd !== undefined) {
    try { closeSync(fd); } catch (error) { closeFailure = error; }
  }
  if (closeFailure && !UNSUPPORTED_DIRECTORY_SYNC.has(closeFailure.code)) return closeFailure;
  return syncFailure && !UNSUPPORTED_DIRECTORY_SYNC.has(syncFailure.code) ? syncFailure : null;
}

function readBounded(fd, maxBytes) {
  const buffer = Buffer.alloc(maxBytes);
  let bytes = 0;
  while (bytes < buffer.length) {
    const count = readSync(fd, buffer, bytes, buffer.length - bytes, null);
    if (!count) break;
    bytes += count;
  }
  return buffer.subarray(0, bytes);
}
