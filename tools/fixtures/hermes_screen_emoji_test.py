"""Current signed-session/card color glyphs; no model calls or physical device."""
import asyncio
import base64
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from aiohttp import WSMsgType

from hermes_test_support import DeviceSocket, KubikAdapter, Peer, ProtocolFixture, atomic_json
from hermes_color_emoji_support import KEYS, atlas
from hermes_emoji_support import GOLDEN_KEYS
from hermes_kubik.screen_emoji import ColorAtlas, ScreenEmoji, display_text, parse_color, request


class ColorFormatTests(unittest.TestCase):
    def test_palette_and_alias_contract(self):
        self.assertEqual(hashlib.sha256(atlas(GOLDEN_KEYS)).hexdigest(), '649a4e4f1bfce244d4f0a60e6e26eb0ffe980d31bfb1787f667ae4a6b22b3170')
        self.assertEqual(len(parse_color(atlas())), len(KEYS))
        self.assertEqual(len(set(parse_color(atlas(alias=True)).values())), 1)
        data = bytearray(atlas(['😀']))
        glyph = struct.unpack_from('<I', data, 36)[0]
        for offset, value in ((glyph, 1), (glyph + 7, 0), (glyph + 8, 9), (glyph + 11, 255)):
            changed = bytearray(data); changed[offset] = value
            with self.assertRaises(ValueError):
                parse_color(changed)
        data[glyph + 11] = 0  # Unused transparent palette entries remain valid.
        self.assertIn('😀', parse_color(data))
        data[glyph:glyph + 576] = bytes(576)
        with self.assertRaises(ValueError):
            parse_color(data)

    def test_regions_flags_keys_and_exact_length_are_strict(self):
        for offset, fmt, value in ((0, '<4s', b'KEMJ'), (4, '<I', 2), (8, '<I', 16),
                                    (12, '<I', 0), (12, '<I', 8193), (16, '<I', 36),
                                    (20, '<I', 33), (24, '<I', 33), (28, '<I', 32),
                                    (32, '<I', 31), (36, '<I', 33), (40, '<H', 65), (42, '<H', 1)):
            data = bytearray(atlas())
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                parse_color(data)
        for key in ('\0', '\n'):
            with self.assertRaises(ValueError):
                parse_color(atlas([key]))
        with self.assertRaises(ValueError):
            parse_color(atlas() + b'extra')

    def test_missing_corrupt_oversized_and_symlink_files_do_not_reuse_old_glyphs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'color.bin'
            store = ColorAtlas(path)
            self.assertEqual(store.load(), {})
            path.write_bytes(atlas())
            self.assertIn('😀', store.load())
            path.write_bytes(b'broken')
            self.assertEqual(store.load(), {})
            with path.open('wb') as output:
                output.truncate(8 * 1024 * 1024 + 1)
            self.assertEqual(store.load(), {})
            path.unlink(); path.symlink_to(Path(directory) / 'missing')
            self.assertEqual(store.load(), {})

    def test_grapheme_safe_display_keeps_emoji_only_and_does_not_split_composites(self):
        self.assertEqual(display_text('😀❤️👍🏽👩‍💻🇷🇺'), '😀❤️👍🏽👩‍💻🇷🇺')
        for sequence in ('❤️', '👍🏽', '👩‍💻', '🇷🇺'):
            prefix = 'a' * (1000 - len(sequence.encode()) + 1)
            self.assertEqual(display_text(prefix + sequence), prefix)
        self.assertEqual(len(display_text('я' * 600).encode()), 1000)

    def test_request_shape_whole_sequences_and_positive_uint32(self):
        good = {'t': 'emoji_get', 'id': 1, 'seq': ['😀', '❤️', '👍🏽', '👩‍💻']}
        self.assertTrue(request(good))
        for changed in ({**good, 'id': True}, {**good, 'id': 0}, {**good, 'id': 2**32},
                        {**good, 'seq': []}, {**good, 'seq': KEYS[:5]},
                        {**good, 'seq': ['😀', '😀']}, {**good, 'seq': ['😀😀']},
                        {**good, 'seq': ['😀\u200d']}, {**good, 'seq': ['🏽']}, {**good, 'seq': ['\ufe0f']},
                        {**good, 'seq': ['text']}, {**good, 'extra': 1}):
            self.assertFalse(request(changed))


class ColorReplies(ProtocolFixture):
    def setUp(self):
        super().setUp()
        self.adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        self.adapter.security = self.store
        self.identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [self.identity])
        self.peer = Peer(DeviceSocket(), self.identity, {**self.hello, 'screen_emoji': 1})
        self.peer.ready = True
        self.adapter.peers[self.identity] = self.peer
        self.handler = self.adapter.screen_emoji
        self.handler.atlas.path.parent.mkdir(parents=True, exist_ok=True)
        self.handler.atlas.path.write_bytes(atlas())
        self.handler.text(self.peer, 'Apple ' + ' '.join(KEYS))

    async def get(self, identifier=1, sequences=None, peer=None, *, wait=True):
        peer = peer or self.peer
        await self.adapter.command(peer, {'t': 'emoji_get', 'id': identifier, 'seq': sequences or ['😀']})
        state = self.handler.states.get(peer)
        if wait and state and state['task']:
            await state['task']

    async def test_current_card_exact_rgba_index_payload_and_unrelated_or_missing_null(self):
        await self.get(sequences=['😀', '❤️', '🦄'])
        replies = self.peer.socket.messages
        self.assertEqual([row['seq'] for row in replies], ['😀', '❤️', '🦄'])
        self.assertEqual(len(base64.b64decode(replies[0]['data'], validate=True)), 576)
        self.assertIsNone(replies[2]['data'])
        self.handler.text(self.peer, 'No emoji')
        await self.get(2)
        self.assertIsNone(self.peer.socket.messages[-1]['data'])

    async def test_welcome_revocation_replacement_and_other_profile_cannot_leak(self):
        self.peer.ready = False
        await self.get()
        self.assertFalse(self.peer.socket.messages)
        self.peer.ready = True
        atomic_json(self.store.allow_path, [])
        await self.get()
        self.assertFalse(self.peer.socket.messages)
        atomic_json(self.store.allow_path, [self.identity])
        replacement = Peer(DeviceSocket(), self.identity, {**self.hello, 'screen_emoji': 1})
        replacement.ready = True
        self.adapter.peers[self.identity] = replacement
        await self.get()
        self.assertFalse(self.peer.socket.messages)
        await self.get(peer=replacement)
        self.assertIsNone(replacement.socket.messages[-1]['data'])

    async def test_bucket_excess_gets_null_and_refills_without_closing_voice(self):
        now = [0.]
        self.handler.now = lambda: now[0]
        for index in range(5):
            await self.get(index + 1)
        self.assertTrue(all(row['data'] for row in self.peer.socket.messages[:4]))
        self.assertIsNone(self.peer.socket.messages[-1]['data'])
        self.assertFalse(self.peer.socket.closed)
        now[0] = .5
        await self.get(6)
        self.assertIsNotNone(self.peer.socket.messages[-1]['data'])

    async def test_new_page_coalesces_and_source_change_fences_old_load(self):
        entered, release = asyncio.Event(), asyncio.Event()
        async def lookup():
            entered.set(); await release.wait()
            return parse_color(atlas())
        self.handler.lookup = lookup
        first = asyncio.create_task(self.get(1, ['😀']))
        await entered.wait()
        await self.get(2, ['❤️'], wait=False)
        release.set(); await first
        self.assertEqual([(row['id'], row['seq']) for row in self.peer.socket.messages], [(2, '❤️')])
        entered.clear(); release.clear(); self.peer.socket.messages.clear()
        stale = asyncio.create_task(self.get(3, ['😀']))
        await entered.wait()
        self.handler.text(self.peer, 'New text without emoji')
        release.set(); await stale
        self.assertFalse(self.peer.socket.messages)

    async def test_disconnect_during_load_and_malformed_request(self):
        entered, release = asyncio.Event(), asyncio.Event()
        async def lookup():
            entered.set(); await release.wait(); return parse_color(atlas())
        self.handler.lookup = lookup
        await self.get(wait=False)
        pending = self.handler.states[self.peer]['task']
        await entered.wait()
        await self.peer.socket.close()
        await self.handler.close(self.peer)
        release.set()
        self.assertTrue(pending.cancelled())
        self.assertFalse(self.peer.socket.messages)
        with self.assertRaises(ValueError):
            await self.adapter.command(self.peer, {'t': 'emoji_get', 'id': False, 'seq': ['😀']})

    async def test_slow_color_lookup_never_blocks_actual_binary_reader_path(self):
        entered, release, microphone = asyncio.Event(), asyncio.Event(), []
        async def lookup():
            entered.set(); await release.wait(); return parse_color(atlas())
        self.handler.lookup = lookup
        self.peer.native = SimpleNamespace(microphone=microphone.append)
        text = SimpleNamespace(type=WSMsgType.TEXT, data=json.dumps({'t': 'emoji_get', 'id': 1, 'seq': ['😀']}))
        await asyncio.wait_for(self.adapter._handle_frame(self.peer, text), .2)
        await entered.wait()
        binary = SimpleNamespace(type=WSMsgType.BINARY, data=b'actual-reader-microphone-frame')
        await asyncio.wait_for(self.adapter._handle_frame(self.peer, binary), .2)
        self.assertEqual(microphone, [binary.data])
        self.assertFalse(release.is_set())
        self.assertFalse(self.handler.states[self.peer]['task'].done())
        release.set()
        await self.handler.states[self.peer]['task']

    async def test_outbound_buffer_is_bounded_and_emoji_only_text_needs_no_tts(self):
        self.peer.transport = SimpleNamespace(get_write_buffer_size=lambda: 65536)
        await self.get()
        self.assertFalse(self.peer.socket.messages)
        self.peer.transport = None
        self.peer.volume = 0
        original = self.peer.socket.send_json
        async def send(value, **kwargs):
            await original(value, **kwargs)
            if value['t'] == 'text':
                await self.adapter.command(self.peer, {'t': 'shown', 'receipt': value['receipt']})
        self.peer.socket.send_json = send
        text = '😀❤️👍🏽👩‍💻🇷🇺'
        self.assertTrue((await self.adapter.send(self.identity, text)).success)
        self.assertEqual(self.peer.socket.messages[-1]['text'], text)
        self.assertFalse(self.adapter._should_auto_tts_for_chat(self.identity))
