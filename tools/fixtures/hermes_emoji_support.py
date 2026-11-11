"""Public synthetic patterns for KEMJ validation; contains no Apple artwork."""
import struct

GOLDEN_KEYS = ['1️⃣', '❤️', '👍🏽', '🇷🇺', '👨‍👩‍👧‍👦', '😀']
GOLDEN_SHA256 = 'cf060e21a44e0901f043500367ab139695f590821dd8f442167371cbe2e66b7b'


def atlas(keys=GOLDEN_KEYS, *, alias=False):
    encoded = sorted(key.encode('utf-8') for key in keys)
    keys_at = 32 + 12 * len(encoded)
    bitmaps_at = (keys_at + sum(map(len, encoded)) + 3) & ~3
    size = bitmaps_at + 128 * (1 if alias else len(encoded))
    data = bytearray(size)
    struct.pack_into('<4s7I', data, 0, b'KEMJ', 1, 32, len(encoded), 32, keys_at, bitmaps_at, size)
    offset = keys_at
    for index, key in enumerate(encoded):
        bitmap = bitmaps_at + (0 if alias else 128 * index)
        struct.pack_into('<IIHH', data, 32 + 12 * index, offset, bitmap, len(key), 0)
        data[offset:offset + len(key)] = key
        data[bitmap:bitmap + 128] = bytes([1 if alias else index + 1]) * 128
        offset += len(key)
    return bytes(data)
