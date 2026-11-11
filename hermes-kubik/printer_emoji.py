"""Read a private KEMJ atlas; transport only exact, used whole emoji graphemes."""
import base64
import os
from pathlib import Path
import stat
import struct
import unicodedata

import regex

MAX_BYTES = 2 * 1024 * 1024
CLUSTERS = regex.compile(r'\X', regex.VERSION1)
EMOJI = regex.compile(r'[\p{Extended_Pictographic}\p{Regional_Indicator}\p{Emoji_Presentation}\u20e3\ufe0f]')


class EmojiError(ValueError):
    def __init__(self, code='unsupported_character'):
        super().__init__(code)
        self.code = code


def header(data):
    if not 32 <= len(data) <= MAX_BYTES or data[:4] != b'KEMJ':
        raise ValueError('Invalid KEMJ header')
    version, edge, count, index, keys, bitmaps, size = struct.unpack_from('<7I', data, 4)
    if (version, edge, index) != (1, 32, 32) or not 1 <= count <= 8192:
        raise ValueError('Invalid KEMJ profile')
    if keys != 32 + 12 * count or not keys <= bitmaps <= size or bitmaps % 4 or size != len(data):
        raise ValueError('Invalid KEMJ regions')
    return count, keys, bitmaps


def entry(data, offset, keys, bitmaps):
    key, bitmap, length, flags = struct.unpack_from('<IIHH', data, offset)
    if flags or not 1 <= length <= 64 or not keys <= key or key + length > bitmaps:
        raise ValueError('Invalid KEMJ key region')
    if bitmap < bitmaps or bitmap % 4 or bitmap + 128 > len(data):
        raise ValueError('Invalid KEMJ bitmap region')
    raw = data[key:key + length]
    text = raw.decode('utf-8', errors='strict')
    if any(unicodedata.category(char) == 'Cc' for char in text):
        raise ValueError('Invalid KEMJ key text')
    return raw, text, data[bitmap:bitmap + 128]


def parse(data):
    count, keys, bitmaps = header(data)
    result, previous = {}, b''
    for index in range(count):
        raw, text, bitmap = entry(data, 32 + 12 * index, keys, bitmaps)
        if raw <= previous:
            raise ValueError('KEMJ keys must be uniquely byte-sorted')
        previous = raw
        result[text] = bitmap
    return result


def read_regular(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as source:
        metadata = os.fstat(source.fileno())
        if not stat.S_ISREG(metadata.st_mode) or not 32 <= metadata.st_size <= MAX_BYTES:
            raise ValueError('Invalid KEMJ file')
        return source.read(MAX_BYTES + 1)


class EmojiAtlas:
    def __init__(self, path):
        self.path, self.identity, self.entries = Path(path), None, {}

    def load(self):
        metadata = self.path.stat(follow_symlinks=False)
        identity = (metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns, metadata.st_ctime_ns)
        if identity != self.identity:
            self.entries = parse(read_regular(self.path))
            self.identity = identity
        return self.entries

    def glyphs(self, text):
        selected = dict.fromkeys(cluster for cluster in CLUSTERS.findall(text) if EMOJI.search(cluster))
        if not selected:
            return []
        if len(selected) > 8:
            raise EmojiError('invalid_request')
        try:
            atlas = self.load()
            return [{'sequence': key, 'bitmap': base64.b64encode(atlas[key]).decode('ascii')} for key in selected]
        except (OSError, ValueError, KeyError, struct.error):
            raise EmojiError() from None
