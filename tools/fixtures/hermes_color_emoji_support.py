"""Synthetic KEMC palette/index data; never Apple artwork."""
import struct

KEYS = ['😀', '❤️', '👍🏽', '👩‍💻', '🇷🇺', '😎', '🍎', '🚀', '🐱', '✅']


def atlas(keys=KEYS, *, alias=False):
    encoded = sorted(key.encode() for key in keys)
    keys_at = 32 + 12 * len(encoded)
    glyphs_at = (keys_at + sum(map(len, encoded)) + 3) & ~3
    size = glyphs_at + 576 * (1 if alias else len(encoded))
    data = bytearray(size)
    struct.pack_into('<4s7I', data, 0, b'KEMC', 1, 32, len(encoded), 32, keys_at, glyphs_at, size)
    key_at = keys_at
    for index, key in enumerate(encoded):
        glyph_at = glyphs_at + (0 if alias else 576 * index)
        struct.pack_into('<IIHH', data, 32 + 12 * index, key_at, glyph_at, len(key), 0)
        data[key_at:key_at + len(key)] = key
        data[glyph_at + 4:glyph_at + 8] = bytes([index + 1, 40, 80, 255])
        data[glyph_at + 64:glyph_at + 576] = b'\x11' * 512
        key_at += len(key)
    return bytes(data)
