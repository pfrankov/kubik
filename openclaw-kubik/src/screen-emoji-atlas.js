import { constants } from 'node:fs';
import { open } from 'node:fs/promises';
import { join } from 'node:path';

const MAX_BYTES = 8 * 1024 * 1024;
const GLYPH_BYTES = 576;
const MAGIC = Buffer.from('KEMC');
const utf8 = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
const invalid = () => { throw Error('Invalid private color emoji atlas'); };
const bounded = bytes => Buffer.isBuffer(bytes) && bytes.length >= 32 && bytes.length <= MAX_BYTES && bytes.subarray(0, 4).equals(MAGIC);

function header(bytes) {
  if (!bounded(bytes)) invalid();
  const [version, edge, count, index, keys, glyphs, size] = Array.from({ length: 7 }, (_, i) => bytes.readUInt32LE(4 + i * 4));
  if (version !== 1 || edge !== 32 || count < 1 || count > 8192 || index !== 32 || keys !== 32 + count * 12 ||
      glyphs < keys || glyphs % 4 || glyphs > bytes.length || size !== bytes.length) invalid();
  return { count, keys, glyphs };
}

function record(bytes, index, layout) {
  const at = 32 + index * 12;
  const keyAt = bytes.readUInt32LE(at), glyphAt = bytes.readUInt32LE(at + 4);
  const length = bytes.readUInt16LE(at + 8), flags = bytes.readUInt16LE(at + 10);
  if (flags || length < 1 || length > 64 || keyAt < layout.keys || keyAt + length > layout.glyphs ||
      glyphAt < layout.glyphs || glyphAt % 4 || glyphAt + GLYPH_BYTES > bytes.length) invalid();
  const key = bytes.subarray(keyAt, keyAt + length), sequence = utf8.decode(key);
  if (/\p{Cc}/u.test(sequence)) invalid();
  return { key, sequence, glyphAt };
}

function validGlyph(glyph) {
  if (glyph.readUInt32LE(0) !== 0) return false;
  let used = 1;
  for (const pixels of glyph.subarray(64)) {
    used |= (1 << (pixels >> 4)) | (1 << (pixels & 15));
  }
  for (let index = 1; index < 16; index++)
    if (used & (1 << index) ? !glyph[index * 4 + 3] : glyph.readUInt32LE(index * 4) !== 0) return false;
  return (used & 0xfffe) !== 0;
}

/** KEMC is private artwork; every referenced palette entry has visible straight alpha. */
export function parseColorEmojiAtlas(bytes) {
  const layout = header(bytes), entries = new Map(), checked = new Set();
  let previous = null;
  for (let index = 0; index < layout.count; index++) {
    const item = record(bytes, index, layout);
    if (previous && Buffer.compare(previous, item.key) >= 0) invalid();
    const glyph = bytes.subarray(item.glyphAt, item.glyphAt + GLYPH_BYTES);
    if (!checked.has(item.glyphAt) && !validGlyph(glyph)) invalid();
    checked.add(item.glyphAt); entries.set(item.sequence, glyph); previous = item.key;
  }
  return entries;
}

/** A shared, bounded account cache; concurrent readers share one read, never one atlas per request. */
export class ColorEmojiAtlas {
  #cached = null;
  #reading = null;
  constructor(stateDir) { this.path = stateDir ? join(stateDir, 'apple-emoji-color.bin') : null; }

  async load() {
    if (!this.path) return null;
    if (this.#reading) return this.#reading;
    const read = this.#read(); this.#reading = read;
    try { return await read; } finally { if (this.#reading === read) this.#reading = null; }
  }

  async #read() {
    let file;
    try {
      file = await open(this.path, constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK);
      const stat = await file.stat({ bigint: true });
      if (!stat.isFile() || stat.size < 32n || stat.size > BigInt(MAX_BYTES)) invalid();
      const identity = [stat.dev, stat.ino, stat.size, stat.mtimeNs, stat.ctimeNs].join(':');
      if (this.#cached?.identity === identity) return this.#cached.entries;
      this.#cached = null;
      const bytes = Buffer.alloc(Number(stat.size));
      if ((await file.read(bytes, 0, bytes.length, 0)).bytesRead !== bytes.length) invalid();
      const entries = parseColorEmojiAtlas(bytes); this.#cached = { identity, entries }; return entries;
    } catch { this.#cached = null; return null; }
    finally { await file?.close(); }
  }
}
