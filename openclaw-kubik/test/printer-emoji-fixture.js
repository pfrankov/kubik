/** Synthetic geometric data only: this fixture contains no Apple artwork. */
export const GOLDEN_SEQUENCES = ['1️⃣', '❤️', '👍🏽', '🇷🇺', '👨‍👩‍👧‍👦', '😀'];

export function syntheticAtlas(sequences = GOLDEN_SEQUENCES) {
  const keys = [...sequences].map(value => Buffer.from(value)).sort(Buffer.compare);
  const keysAt = 32 + keys.length * 12;
  const bitmapsAt = (keysAt + keys.reduce((sum, key) => sum + key.length, 0) + 3) & ~3;
  const bytes = Buffer.alloc(bitmapsAt + keys.length * 128);
  bytes.write('KEMJ');
  for (const [i, value] of [1, 32, keys.length, 32, keysAt, bitmapsAt, bytes.length].entries()) bytes.writeUInt32LE(value, 4 + i * 4);
  let keyAt = keysAt;
  for (const [i, key] of keys.entries()) {
    const index = 32 + i * 12;
    bytes.writeUInt32LE(keyAt, index); bytes.writeUInt32LE(bitmapsAt + i * 128, index + 4);
    bytes.writeUInt16LE(key.length, index + 8); key.copy(bytes, keyAt); keyAt += key.length;
    bytes.fill(i + 1, bitmapsAt + i * 128, bitmapsAt + (i + 1) * 128);
  }
  return bytes;
}
