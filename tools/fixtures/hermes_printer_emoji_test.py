"""Private atlas validation, whole emoji matching, and exact bounded wire encoding."""
import base64
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

import hermes_test_support
from hermes_emoji_support import GOLDEN_KEYS, GOLDEN_SHA256, atlas
from hermes_kubik.printer_emoji import EmojiAtlas, EmojiError, parse
from hermes_kubik.printer_wire import dumps, packet


class EmojiAtlasTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / 'apple-emoji.bin'
        self.store = EmojiAtlas(self.path)

    def tearDown(self):
        self.directory.cleanup()

    def test_public_golden_matches_javascript_and_only_used_patterns_are_embedded(self):
        data = atlas()
        self.assertEqual(hashlib.sha256(data).hexdigest(), GOLDEN_SHA256)
        self.path.write_bytes(data)
        values = self.store.glyphs('Apple 😀 ❤️ 😀 👍🏽 🇷🇺')
        self.assertEqual([row['sequence'] for row in values], ['😀', '❤️', '👍🏽', '🇷🇺'])
        self.assertTrue(all(len(row['bitmap']) == 172 and len(base64.b64decode(row['bitmap'], validate=True)) == 128 for row in values))
        self.assertNotIn('👨‍👩‍👧‍👦', [row['sequence'] for row in values])

    def test_plain_text_works_without_atlas_but_missing_emoji_never_substitutes(self):
        self.assertEqual(self.store.glyphs('Hello\nМолоко\t123'), [])
        with self.assertRaisesRegex(EmojiError, 'unsupported_character'):
            self.store.glyphs('😀')
        self.path.write_bytes(b'invalid')
        self.assertEqual(self.store.glyphs('Только текст'), [])
        with self.assertRaises(EmojiError):
            self.store.glyphs('😀')

    def test_unsupported_whole_composites_cannot_split_into_base_glyphs(self):
        self.path.write_bytes(atlas(['👩', '💻', '👍', '❤', '🇷', '🇺', '👨']))
        for sequence in ('👩‍💻', '👍🏽', '❤️', '🇷🇺', '👨‍👩‍👧‍👦', '1️⃣'):
            with self.subTest(sequence=sequence), self.assertRaises(EmojiError):
                self.store.glyphs(sequence)

    def test_used_unique_glyph_limit_is_eight_and_duplicates_do_not_expand_it(self):
        keys = [chr(0x1f600 + index) for index in range(9)]
        self.path.write_bytes(atlas(keys))
        self.assertEqual(len(self.store.glyphs(''.join(keys[:8]) * 5)), 8)
        with self.assertRaisesRegex(EmojiError, 'invalid_request'):
            self.store.glyphs(''.join(keys))

    def test_atomic_replace_refreshes_cache_and_corruption_cannot_reuse_old_artwork(self):
        self.path.write_bytes(atlas(['😀']))
        first = self.store.glyphs('😀')
        replacement = self.path.with_suffix('.new')
        replacement.write_bytes(atlas(['❤️', '😀']))
        replacement.replace(self.path)
        self.assertNotEqual(first, self.store.glyphs('😀'))
        self.path.write_bytes(b'corrupt')
        with self.assertRaises(EmojiError):
            self.store.glyphs('😀')

    def test_alias_bitmap_offsets_are_valid_but_symlinks_and_oversized_files_are_rejected(self):
        entries = parse(atlas(alias=True))
        self.assertEqual(len(set(entries.values())), 1)
        target = self.path.with_suffix('.target')
        target.write_bytes(atlas())
        self.path.symlink_to(target)
        with self.assertRaises(EmojiError):
            self.store.glyphs('😀')
        self.path.unlink()
        with self.path.open('wb') as output:
            output.truncate(2 * 1024 * 1024 + 1)
        with self.assertRaises(EmojiError):
            self.store.glyphs('😀')

    def test_header_and_record_regions_fail_closed(self):
        mutations = [(4, 2), (8, 16), (12, 0), (12, 8193), (16, 36), (20, 33), (24, 33), (28, 32)]
        for offset, number in mutations:
            data = bytearray(atlas())
            struct.pack_into('<I', data, offset, number)
            with self.subTest(offset=offset, number=number), self.assertRaises(ValueError):
                parse(data)
        for offset, fmt, number in ((32, '<I', 31), (36, '<I', 33), (40, '<H', 0), (40, '<H', 65), (42, '<H', 1)):
            data = bytearray(atlas())
            struct.pack_into(fmt, data, offset, number)
            with self.subTest(offset=offset, number=number), self.assertRaises(ValueError):
                parse(data)
        with self.assertRaises(ValueError):
            parse(atlas() + b'junk')

    def test_invalid_utf8_controls_unsorted_and_duplicate_keys_are_rejected(self):
        for key in ('\0', '\n', '\x7f'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                parse(atlas([key]))
        data = bytearray(atlas(['😀']))
        key_at = struct.unpack_from('<I', data, 32)[0]
        data[key_at] = 0xff
        with self.assertRaises(UnicodeDecodeError):
            parse(data)
        data = bytearray(atlas())
        first, second = bytes(data[32:44]), bytes(data[44:56])
        data[32:44], data[44:56] = second, first
        with self.assertRaises(ValueError):
            parse(data)
        data[32:44] = data[44:56]
        with self.assertRaises(ValueError):
            parse(data)

    def test_wire_requires_internal_empty_text_glyphs_and_no_status_or_qr_field(self):
        text = packet('a' * 32, {'action': 'print', 'text': 'Список', 'format': 'text'}, [])
        self.assertEqual(text['emoji'], [])
        self.assertIn('Список', dumps(text))
        self.assertNotIn('\\u', dumps(text))
        self.assertEqual(json.loads(dumps(text)), text)
        self.assertNotIn('emoji', packet('a' * 32, {'action': 'status'}, []))
        self.assertNotIn('emoji', packet('a' * 32, {'action': 'print', 'format': 'qr', 'text': '😀'}, []))
        with self.assertRaises(ValueError):
            packet('a' * 32, {'action': 'print', 'text': 'я' * 4096, 'format': 'text'}, [])


if __name__ == '__main__':
    unittest.main()
