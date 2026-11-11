"""Strict KEMC v1 color glyph payloads; container logic is shared with KEMJ."""
from atlas import decode_payloads, encode_payloads

GLYPH_BYTES = 576
MAX_BYTES = 8 * 1024 * 1024


def validate_payload(payload):
    if not isinstance(payload, bytes) or len(payload) != GLYPH_BYTES:
        raise ValueError('Color emoji payload must contain exactly 576 bytes')
    if payload[:4] != bytes(4):
        raise ValueError('Color palette index zero must be transparent black')
    for offset in range(4, 64, 4):
        color = payload[offset:offset + 4]
        if not color[3] and any(color):
            raise ValueError('Unused transparent palette entries must be zero')
    used = {nibble for byte in payload[64:] for nibble in (byte >> 4, byte & 15)}
    if any(index and not payload[index * 4 + 3] for index in used):
        raise ValueError('A color pixel references an empty palette entry')


def encode(glyphs):
    for payload in glyphs.values():
        validate_payload(payload)
    return encode_payloads(glyphs, b'KEMC', GLYPH_BYTES, MAX_BYTES)


def decode(data):
    glyphs = decode_payloads(data, b'KEMC', GLYPH_BYTES, MAX_BYTES)
    for payload in glyphs.values():
        validate_payload(payload)
    return glyphs


def rgba(payload):
    validate_payload(payload)
    palette = [payload[index:index + 4] for index in range(0, 64, 4)]
    return b''.join(palette[index] for byte in payload[64:] for index in (byte >> 4, byte & 15))
