"""Current-card color emoji: private atlas, bounded requests, no agent or print jobs."""
import asyncio
import base64
import os
from pathlib import Path
import stat
import struct
import time
import unicodedata
from weakref import WeakKeyDictionary
import regex

from .printer_emoji import CLUSTERS

MAX_BYTES = 8 * 1024 * 1024
BASE = regex.compile(r'[\p{Extended_Pictographic}\p{Regional_Indicator}]|[0-9#*]\ufe0f?\u20e3')


def display_text(content):
    chunks, size = [], 0
    for match in CLUSTERS.finditer(str(content)):
        cluster = match.group()
        length = len(cluster.encode('utf-8'))
        if size + length > 1000:
            break
        chunks.append(cluster)
        size += length
    return ''.join(chunks)


def whole(sequence):
    if not isinstance(sequence, str):
        return False
    try:
        size = len(sequence.encode('utf-8'))
    except UnicodeEncodeError:
        return False
    if sequence.startswith('\u200d') or sequence.endswith('\u200d') or '\u200d\u200d' in sequence:
        return False
    if any(unicodedata.category(char) == 'Cc' for char in sequence):
        return False
    return 1 <= size <= 64 and CLUSTERS.fullmatch(sequence) is not None and BASE.search(sequence) is not None


def request(value):
    if not isinstance(value, dict) or value.keys() != {'t', 'id', 'seq'} or value.get('t') != 'emoji_get':
        return False
    sequences, identifier = value['seq'], value['id']
    if type(identifier) is not int or not 1 <= identifier <= 0xffffffff:
        return False
    if not isinstance(sequences, list) or not 1 <= len(sequences) <= 4 or not all(whole(item) for item in sequences):
        return False
    return len(set(sequences)) == len(sequences)


def color_header(data):
    if not 32 <= len(data) <= MAX_BYTES or data[:4] != b'KEMC':
        raise ValueError('Invalid KEMC header')
    version, edge, count, index, keys, glyphs, size = struct.unpack_from('<7I', data, 4)
    if (version, edge, index) != (1, 32, 32) or not 1 <= count <= 8192:
        raise ValueError('Invalid KEMC profile')
    if keys != 32 + 12 * count or not keys <= glyphs <= size or glyphs % 4 or size != len(data):
        raise ValueError('Invalid KEMC regions')
    return count, keys, glyphs


def color_entry(data, offset, keys, glyphs):
    key, glyph, length, flags = struct.unpack_from('<IIHH', data, offset)
    if flags or not 1 <= length <= 64 or key < keys or key + length > glyphs:
        raise ValueError('Invalid KEMC key')
    if glyph < glyphs or glyph % 4 or glyph + 576 > len(data):
        raise ValueError('Invalid KEMC glyph')
    raw = data[key:key + length]
    text = raw.decode('utf-8', errors='strict')
    if any(unicodedata.category(char) == 'Cc' for char in text):
        raise ValueError('Invalid KEMC key text')
    return raw, text, glyph


def valid_palette(glyph):
    if glyph[:4] != b'\0' * 4:
        return False
    packed = set(glyph[64:])
    used = {value >> 4 for value in packed} | {value & 15 for value in packed}
    if not used - {0}:
        return False
    for index in range(1, 16):
        if index in used:
            if glyph[index * 4 + 3] == 0:
                return False
        elif glyph[index * 4:index * 4 + 4] != b'\0' * 4:
            return False
    return True


def parse_color(data):
    count, keys, glyphs = color_header(data)
    result, bitmaps, previous = {}, {}, b''
    for index in range(count):
        raw, text, glyph = color_entry(data, 32 + 12 * index, keys, glyphs)
        if raw <= previous:
            raise ValueError('KEMC keys must be uniquely byte-sorted')
        previous = raw
        if glyph not in bitmaps:
            bitmap = data[glyph:glyph + 576]
            if not valid_palette(bitmap):
                raise ValueError('Invalid KEMC palette alpha')
            bitmaps[glyph] = bitmap
        result[text] = bitmaps[glyph]
    return result


def read_color(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as source:
        metadata = os.fstat(source.fileno())
        if not stat.S_ISREG(metadata.st_mode) or not 32 <= metadata.st_size <= MAX_BYTES:
            raise ValueError('Invalid KEMC file')
        return source.read(MAX_BYTES + 1)


class ColorAtlas:
    def __init__(self, path):
        self.path, self.identity, self.entries = Path(path), None, {}

    def load(self):
        try:
            metadata = self.path.stat(follow_symlinks=False)
            identity = (metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns, metadata.st_ctime_ns)
            if identity != self.identity:
                self.identity, self.entries = identity, {}
                self.entries = parse_color(read_color(self.path))
        except (OSError, ValueError, struct.error):
            self.entries = {}
        return self.entries


class ScreenEmoji:
    def __init__(self, adapter, *, now=time.monotonic):
        self.adapter, self.now, self.states = adapter, now, WeakKeyDictionary()
        self.atlas = ColorAtlas(adapter.home / 'kubik/apple-emoji-color.bin')
        self.loading = None

    def current(self, peer):
        return self.adapter._ready_peer(peer.identity) is peer and bool(self.adapter.config.enabled)

    def state(self, peer):
        if peer not in self.states:
            self.states[peer] = {'tokens': 4., 'at': self.now(), 'task': None, 'page': 0, 'epoch': 0, 'allowed': set(), 'pending': None}
        return self.states[peer]

    def text(self, peer, text):
        state = self.state(peer)
        state['epoch'] += 1
        state['pending'] = None
        state['allowed'] = {cluster for cluster in CLUSTERS.findall(text) if whole(cluster)}

    def admit(self, state):
        now = self.now()
        state['tokens'] = min(4., state['tokens'] + max(0, now - state['at']) * 2)
        state['at'] = now
        accepted = state['tokens'] >= 1
        if accepted:
            state['tokens'] -= 1
        return accepted

    async def lookup(self):
        if self.loading is None or self.loading.done():
            self.loading = asyncio.create_task(asyncio.to_thread(self.atlas.load))
            self.loading.add_done_callback(self.finished)
        return await asyncio.wait_for(asyncio.shield(self.loading), 2)

    async def nulls(self, peer, value, state, source):
        try:
            async with asyncio.timeout(1):
                await self.reply(peer, value, state, source, {})
        except TimeoutError:
            pass

    async def handle(self, peer, value):
        if not request(value):
            raise ValueError('Invalid emoji request')
        if not self.current(peer) or peer.screen_emoji_version != 1:
            return
        state = self.state(peer)
        state['page'] += 1
        state['pending'] = (value, (state['epoch'], state['page']), self.admit(state))
        if state['task'] is None or state['task'].done():
            state['task'] = asyncio.create_task(self.run(peer, state))
            state['task'].add_done_callback(self.finished)

    @staticmethod
    def finished(task):
        if not task.cancelled():
            task.exception()

    async def run(self, peer, state):
        atlas = None
        try:
            async with asyncio.timeout(3):
                while state['pending'] is not None:
                    page, token, accepted = state['pending']
                    if accepted and atlas is None:
                        atlas = await self.lookup()
                        continue  # A new page may have replaced the request during the read.
                    state['pending'] = None
                    await self.reply(peer, page, state, token, atlas if accepted else {})
        except TimeoutError:
            if state['pending'] is not None:
                page, token, _accepted = state['pending']
                await self.nulls(peer, page, state, token)
            state['pending'] = None

    async def reply(self, peer, value, state, source, atlas):
        for sequence in value['seq']:
            if not self.current(peer) or source != (state['epoch'], state['page']):
                return
            if peer.transport is not None and peer.transport.get_write_buffer_size() >= 65536:
                return
            glyph = atlas.get(sequence) if sequence in state['allowed'] else None
            data = base64.b64encode(glyph).decode('ascii') if glyph is not None else None
            await peer.send('emoji', id=value['id'], seq=sequence, data=data)

    async def close(self, peer):
        state = self.states.pop(peer, None)
        if state and state['task'] is not None:
            state['pending'] = None
            state['task'].cancel()
            await asyncio.gather(state['task'], return_exceptions=True)
