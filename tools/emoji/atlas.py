"""Bounded KEMJ v1 private monochrome glyph atlas; no artwork is bundled here."""
import hashlib
import struct

EDGE = 32
GLYPH_BYTES = EDGE * EDGE // 8
MAX_ENTRIES = 8192
MAX_BYTES = 2 * 1024 * 1024
MAX_SEQUENCE_BYTES = 64
HEADER = struct.Struct('<4s7I')
ENTRY = struct.Struct('<IIHH')
UNICODE_URL = 'https://www.unicode.org/Public/17.0.0/emoji/emoji-test.txt'
UNICODE_SHA256 = '1d8a944f88d7952f7ef7c5167fef3c67995bcae24543949710231b03a201acda'


def sequence_bytes(value):
    if not isinstance(value, str):
        raise ValueError('Emoji sequence must be text')
    encoded = value.encode('utf-8', errors='strict')
    if not 1 <= len(encoded) <= MAX_SEQUENCE_BYTES:
        raise ValueError('Emoji sequence exceeds the bounded UTF-8 length')
    if any(ord(char) < 32 or 127 <= ord(char) <= 159 for char in value):
        raise ValueError('Emoji sequence contains a control character')
    return encoded


def parse_sequences(data):
    """Preserve qualified and selector-less aliases; CoreText validates each cluster."""
    result = set()
    for line in data.decode('utf-8').splitlines():
        content = line.partition('#')[0].strip()
        if not content:
            continue
        codes, separator, status = content.partition(';')
        if not separator or status.strip() not in (
                'fully-qualified', 'minimally-qualified', 'unqualified', 'component'):
            raise ValueError('Malformed Unicode emoji-test row')
        value = ''.join(chr(int(code, 16)) for code in codes.split())
        sequence_bytes(value)
        result.add(value)
    if not result or len(result) > MAX_ENTRIES:
        raise ValueError('Unicode emoji sequence count is outside the bound')
    return sorted(result, key=sequence_bytes)


def pinned_sequences(data):
    if hashlib.sha256(data).hexdigest() != UNICODE_SHA256:
        raise ValueError('Unicode emoji-test differs from its pinned SHA-256')
    return parse_sequences(data)


def encode_payloads(glyphs, magic, glyph_bytes, maximum):
    if not 1 <= len(glyphs) <= MAX_ENTRIES:
        raise ValueError('Atlas entry count is outside the bound')
    ordered = sorted(glyphs.items(), key=lambda item: sequence_bytes(item[0]))
    keys = bytearray()
    for key, bitmap in ordered:
        sequence_bytes(key)
        if not isinstance(bitmap, bytes) or len(bitmap) != glyph_bytes:
            raise ValueError('An emoji bitmap has the wrong payload size')
        keys.extend(key.encode('utf-8'))
    keys_offset = HEADER.size + len(ordered) * ENTRY.size
    bits_offset = (keys_offset + len(keys) + 3) & ~3
    records, pixels, offsets = bytearray(), bytearray(), {}
    cursor = keys_offset
    for key, bitmap in ordered:
        if bitmap not in offsets:
            offsets[bitmap] = bits_offset + len(pixels)
            pixels.extend(bitmap)
        encoded = sequence_bytes(key)
        records.extend(ENTRY.pack(cursor, offsets[bitmap], len(encoded), 0))
        cursor += len(encoded)
    size = bits_offset + len(pixels)
    if size > maximum:
        raise ValueError('Atlas exceeds its format size bound')
    header = HEADER.pack(magic, 1, EDGE, len(ordered), HEADER.size,
                         keys_offset, bits_offset, size)
    return header + records + keys + bytes(bits_offset - cursor) + pixels


def _header(data, magic, glyph_bytes, maximum):
    if not HEADER.size <= len(data) <= maximum:
        raise ValueError('Atlas length is outside the bound')
    signature, version, edge, count, index, keys, bits, size = HEADER.unpack_from(data)
    valid = signature == magic and version == 1 and edge == EDGE
    valid &= 1 <= count <= MAX_ENTRIES and index == HEADER.size
    valid &= keys == index + count * ENTRY.size and keys <= bits <= size == len(data)
    valid &= bits % 4 == 0 and (size - bits) % glyph_bytes == 0
    if not valid:
        raise ValueError('Invalid emoji atlas v1 header')
    return count, index, keys, bits


def decode_payloads(data, magic, glyph_bytes, maximum):
    count, index, keys, bits = _header(data, magic, glyph_bytes, maximum)
    result, previous, cursor = {}, b'', keys
    for number in range(count):
        key, bitmap, length, flags = ENTRY.unpack_from(data, index + number * ENTRY.size)
        valid = not flags and key == cursor and 1 <= length <= MAX_SEQUENCE_BYTES
        valid &= key + length <= bits and bitmap >= bits
        valid &= (bitmap - bits) % glyph_bytes == 0 and bitmap + glyph_bytes <= len(data)
        if not valid:
            raise ValueError('Invalid atlas index entry')
        encoded = data[key:key + length]
        value = encoded.decode('utf-8', errors='strict')
        if sequence_bytes(value) != encoded or encoded <= previous:
            raise ValueError('Atlas keys must be unique and UTF-8 lexicographically sorted')
        result[value] = data[bitmap:bitmap + glyph_bytes]
        previous, cursor = encoded, key + length
    if bits - cursor > 3 or any(data[cursor:bits]):
        raise ValueError('Atlas key padding is not canonical')
    return result


def encode(glyphs):
    return encode_payloads(glyphs, b'KEMJ', GLYPH_BYTES, MAX_BYTES)


def decode(data):
    return decode_payloads(data, b'KEMJ', GLYPH_BYTES, MAX_BYTES)
