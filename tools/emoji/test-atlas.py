#!/usr/bin/env python3
"""Synthetic atlas and pinned-input checks; no Apple artwork is committed."""
import base64
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from atlas import decode, encode, ENTRY, GLYPH_BYTES, HEADER, parse_sequences, pinned_sequences

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('build_apple', HERE / 'build-apple.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
INK = bytes([0x80]) + bytes(GLYPH_BYTES - 1)
OTHER = bytes([0x01]) + bytes(GLYPH_BYTES - 1)


class AtlasTests(unittest.TestCase):
    def test_canonical_sort_dedup_and_byte_identical_order(self):
        glyphs = {'👩‍💻': INK, '😀': OTHER, '❤️': INK, '❤': INK, '👍🏽': OTHER, '🇷🇺': INK}
        encoded = encode(glyphs)
        self.assertEqual(decode(encoded), glyphs)
        self.assertEqual(encoded, encode(dict(reversed(list(glyphs.items())))))
        header = HEADER.unpack_from(encoded)
        self.assertEqual(header[:5], (b'KEMJ', 1, 32, 6, 32))
        self.assertEqual(len(encoded) - header[6], 2 * GLYPH_BYTES)
        keys = sorted(glyphs, key=lambda item: item.encode())
        records = [ENTRY.unpack_from(encoded, 32 + i * 12) for i in range(6)]
        self.assertEqual(records[keys.index('❤️')][1], records[keys.index('❤')][1])

    def test_header_bounds_version_dimensions_and_alignment(self):
        original = encode({'😀': INK})
        for index, value in ((1, 2), (2, 64), (3, 0), (3, 8193), (4, 36), (5, 43), (6, 45), (7, len(original) + 1)):
            fields = list(HEADER.unpack_from(original))
            fields[index] = value
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                decode(HEADER.pack(*fields) + original[32:])
        for value in (b'', original[:31], original[:-1], original + b'\0', b'x' * (2 * 1024 * 1024 + 1)):
            with self.assertRaises(ValueError):
                decode(value)

    def test_invalid_index_key_bitmap_and_reserved_flags(self):
        original = encode({'😀': INK})
        record = ENTRY.unpack_from(original, 32)
        for index, value in ((0, 31), (0, record[0] + 1), (1, 0), (1, record[1] + 1), (2, 0), (2, 65), (3, 1)):
            fields = list(record)
            fields[index] = value
            modified = original[:32] + ENTRY.pack(*fields) + original[44:]
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                decode(modified)

    def test_duplicate_keys_bad_utf8_controls_and_noncanonical_padding(self):
        original = bytearray(encode({'😀': INK, '😁': OTHER}))
        first, second = (ENTRY.unpack_from(original, 32 + i * 12) for i in range(2))
        original[second[0]:second[0] + second[2]] = original[first[0]:first[0] + first[2]]
        with self.assertRaises(ValueError):
            decode(original)
        original = bytearray(encode({'😀': INK}))
        key = ENTRY.unpack_from(original, 32)[0]
        original[key] = 0xff
        with self.assertRaises(ValueError):
            decode(original)
        for key in ('', '\0', '\n', 'x' * 65, '\ud800'):
            with self.assertRaises(ValueError):
                encode({key: INK})
        original = bytearray(encode({'❤': INK}))
        bits = HEADER.unpack_from(original)[6]
        original[bits - 1] = 1
        with self.assertRaises(ValueError):
            decode(original)

    def test_exact_glyph_payload_and_bounded_count(self):
        for glyphs in ({}, {'😀': INK[:-1]}, {'😀': bytearray(INK)}, {'😀': INK + b'x'}):
            with self.assertRaises(ValueError):
                encode(glyphs)
        with self.assertRaises(ValueError):
            encode({str(i): INK for i in range(8193)})

    def test_unicode_qualification_and_pin_are_explicit(self):
        data = ('# Emoji data\n1F600 ; fully-qualified # face\n2764 FE0F ; fully-qualified # heart\n'
                '2764 ; unqualified # heart\n1F3FD ; component # skin\n'
                '1F469 200D 1F4BB ; minimally-qualified # coder\n').encode()
        self.assertEqual(set(parse_sequences(data)), {'😀', '❤️', '❤', '🏽', '👩‍💻'})
        with self.assertRaisesRegex(ValueError, 'pinned SHA'):
            pinned_sequences(data)
        for value in (b'1F600 ; invented # no\n', b'not a row\n', b'110000 ; component\n', b''):
            with self.assertRaises(ValueError):
                parse_sequences(value)

    def test_private_output_rejects_repository_and_symlink(self):
        with self.assertRaisesRegex(ValueError, 'outside'):
            builder.private_destination(builder.ROOT / '.tmp/private-apple.bin')
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'target'
            target.write_bytes(b'original')
            link = Path(directory) / 'link'
            link.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                builder.private_destination(link)
            self.assertEqual(target.read_bytes(), b'original')

    def test_private_write_replaces_atomically_and_preserves_previous_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'apple-emoji.bin'
            target.write_bytes(b'previous')
            with patch.object(builder.os, 'replace', side_effect=OSError('simulated failure')):
                with self.assertRaises(OSError):
                    builder.private_write(target, b'new')
            self.assertEqual(target.read_bytes(), b'previous')
            self.assertEqual(list(Path(directory).iterdir()), [target])
            builder.private_write(target, b'complete')
            self.assertEqual(target.read_bytes(), b'complete')
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)

    def test_renderer_result_rejects_fallback_duplicate_and_omission(self):
        value = {'postscript': 'AppleColorEmoji', 'fontURL': str(builder.FONT), 'edge': 32,
                 'glyphs': [{'sequence': '😀', 'bitmap': base64.b64encode(INK).decode()}],
                 'unsupported': [{'sequence': 'A', 'reason': 'fallback_font'}]}
        self.assertEqual(builder.checked_render(value, ['😀', 'A']), {'😀': INK})
        for field, changed in (('postscript', 'Helvetica'), ('edge', 64), ('fontURL', '/tmp/other.ttf')):
            with self.assertRaises(ValueError):
                builder.checked_render({**value, field: changed}, ['😀', 'A'])
        with self.assertRaises(ValueError):
            builder.checked_render(value, ['😀', 'A', '😁'])
        with self.assertRaises(ValueError):
            builder.checked_render({**value, 'unsupported': value['unsupported'] * 2}, ['😀', 'A'])

    @unittest.skipUnless(sys.platform == 'darwin', 'CoreText only runs on macOS')
    def test_actual_coretext_rejects_font_fallback_and_partial_cluster(self):
        requests = ['😀', '❤️', '👍🏽', '👩‍💻', '🇷🇺', '👨‍👩‍👧‍👦', '👨‍❤️‍👨', 'A', '😀\u200dA', '😀\u200d😁']
        with tempfile.TemporaryDirectory() as directory:
            source, result, preview = (Path(directory) / name for name in ('input.json', 'result.json', 'preview.png'))
            source.write_text(json.dumps(requests))
            subprocess.run(['swift', str(HERE / 'render-apple.swift'), str(source), str(result), str(preview)],
                           check=True, capture_output=True, timeout=30)
            output = json.loads(result.read_text())
        glyphs = builder.checked_render(output, requests)
        self.assertEqual(set(glyphs), set(requests[:7]))
        rejected = {row['sequence']: row['reason'] for row in output['unsupported']}
        self.assertEqual(rejected['A'], 'fallback_font')
        self.assertEqual(rejected['😀\u200dA'], 'not_one_grapheme')
        self.assertEqual(rejected['😀\u200d😁'], 'not_one_composed_cell')


if __name__ == '__main__':
    unittest.main()
