export const COLOR_SEQUENCES = ['1️⃣', '❤️', '👍🏽', '🇷🇺', '👨‍👩‍👧‍👦', '😀'];

/** Synthetic color squares only, never Apple artwork. */
export function syntheticColorAtlas(sequences = COLOR_SEQUENCES) {
  const keys = [...sequences].map(value => Buffer.from(value)).sort(Buffer.compare);
  const keysAt = 32 + keys.length * 12;
  const glyphsAt = (keysAt + keys.reduce((sum, key) => sum + key.length, 0) + 3) & ~3;
  const bytes = Buffer.alloc(glyphsAt + keys.length * 576); bytes.write('KEMC');
  for (const [i, value] of [1, 32, keys.length, 32, keysAt, glyphsAt, bytes.length].entries()) bytes.writeUInt32LE(value, 4 + i * 4);
  let keyAt = keysAt;
  for (const [i, key] of keys.entries()) {
    const index = 32 + i * 12, glyph = glyphsAt + i * 576;
    bytes.writeUInt32LE(keyAt, index); bytes.writeUInt32LE(glyph, index + 4); bytes.writeUInt16LE(key.length, index + 8);
    key.copy(bytes, keyAt); keyAt += key.length;
    bytes.set([i + 1, 40, 80, 255], glyph + 4); bytes.fill(0x11, glyph + 64, glyph + 576);
  }
  return bytes;
}
