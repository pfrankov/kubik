import { closeSync, constants, fstatSync, openSync, readSync } from 'node:fs';
import { join } from 'node:path';
import { PrinterError } from './printer-contract.js';

const MAX_ATLAS_BYTES = 2 * 1024 * 1024;
const BITMAP_BYTES = 128;
const MAGIC = Buffer.from('KEMJ');
const segmenter = new Intl.Segmenter('und', { granularity: 'grapheme' });
const emoji = /[\p{Extended_Pictographic}\p{Regional_Indicator}\p{Emoji_Presentation}\u20e3\ufe0f]/u;
const utf8 = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
const invalidAtlas = () => { throw Error('Invalid private emoji atlas'); };
const boundedAtlas = bytes => Buffer.isBuffer(bytes) && bytes.length >= 32 && bytes.length <= MAX_ATLAS_BYTES && bytes.subarray(0, 4).equals(MAGIC);

function atlasHeader(bytes) {
  if (!boundedAtlas(bytes)) invalidAtlas();
  const [version, edge, count, index, keys, bitmaps, size] = Array.from({ length: 7 }, (_, i) => bytes.readUInt32LE(4 + i * 4));
  if (version !== 1 || edge !== 32 || count < 1 || count > 8192 || index !== 32 || keys !== 32 + 12 * count ||
      bitmaps < keys || bitmaps % 4 || bitmaps > bytes.length || size !== bytes.length) invalidAtlas();
  return { count, keys, bitmaps };
}

function atlasRecord(bytes, index, header) {
  const at = 32 + index * 12;
  const keyAt = bytes.readUInt32LE(at), bitmapAt = bytes.readUInt32LE(at + 4);
  const length = bytes.readUInt16LE(at + 8), flags = bytes.readUInt16LE(at + 10);
  if (flags || length < 1 || length > 64 || keyAt < header.keys || keyAt + length > header.bitmaps ||
      bitmapAt < header.bitmaps || bitmapAt % 4 || bitmapAt + BITMAP_BYTES > bytes.length) invalidAtlas();
  const key = bytes.subarray(keyAt, keyAt + length);
  const sequence = utf8.decode(key);
  if (/\p{Cc}/u.test(sequence)) invalidAtlas();
  return { key, sequence, bitmap: bytes.subarray(bitmapAt, bitmapAt + BITMAP_BYTES) };
}

/** KEMJ contains private personal artwork. Only its bounded binary format is part of the plugin. */
export function parseAppleEmojiAtlas(bytes) {
  const header = atlasHeader(bytes), entries = new Map();
  let previous = null;
  for (let index = 0; index < header.count; index++) {
    const entry = atlasRecord(bytes, index, header);
    if (previous && Buffer.compare(previous, entry.key) >= 0) invalidAtlas();
    entries.set(entry.sequence, entry.bitmap); previous = entry.key;
  }
  return entries;
}

/** One account-owned file, at most 2 MiB; replacement or invalidation never reuses stale artwork. */
export class AppleEmojiAtlas {
  #cached = null;
  constructor(stateDir) { this.path = stateDir ? join(stateDir, 'apple-emoji.bin') : null; }

  load() {
    if (!this.path) return null;
    let fd;
    try {
      fd = openSync(this.path, constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK);
      const stat = fstatSync(fd, { bigint: true });
      if (!stat.isFile() || stat.size < 32n || stat.size > BigInt(MAX_ATLAS_BYTES)) invalidAtlas();
      const identity = [stat.dev, stat.ino, stat.size, stat.mtimeNs, stat.ctimeNs].join(':');
      if (this.#cached?.identity === identity) return this.#cached.entries;
      this.#cached = null;
      const bytes = Buffer.alloc(Number(stat.size));
      if (readSync(fd, bytes, 0, bytes.length, 0) !== bytes.length) invalidAtlas();
      const entries = parseAppleEmojiAtlas(bytes);
      this.#cached = { identity, entries }; return entries;
    } catch { this.#cached = null; return null; }
    finally { if (fd !== undefined) closeSync(fd); }
  }
}

function usedEmoji(text, atlas) {
  const used = new Map();
  let entries;
  for (const { segment } of segmenter.segment(text)) {
    if (!emoji.test(segment) || used.has(segment)) continue;
    if (used.size === 8) throw new PrinterError('invalid_request');
    entries ??= atlas?.load();
    const bitmap = entries?.get(segment);
    if (!bitmap) throw new PrinterError('unsupported_character');
    used.set(segment, { sequence: segment, bitmap: bitmap.toString('base64') });
  }
  return [...used.values()];
}

/** Internal wire enrichment: model arguments stay text-only and never select paths or bitmaps. */
export function preparePrinterRequest(request, atlas) {
  if (Object.hasOwn(request, 'emoji')) throw new PrinterError('invalid_request');
  return request.action === 'print' && (request.format ?? 'text') === 'text' ?
    { ...request, emoji: usedEmoji(request.text, atlas) } : { ...request };
}
