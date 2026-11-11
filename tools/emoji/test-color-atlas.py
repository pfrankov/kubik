#!/usr/bin/env python3
"""Synthetic KEMC bounds, palette, quantization and shared KEMJ regressions."""
import struct
import unittest
import zlib

import atlas
import color_atlas
from color_preview import contact_sheet, png
from color_quant import quantize


def pixels(colors):
    return bytes(channel for index in range(1024) for channel in colors[index % len(colors)])


class ColorAtlasTests(unittest.TestCase):
    def test_exact_colors_alpha_and_high_nibble_order(self):
        source = pixels([(255, 32, 10, 255), (16, 100, 230, 64), (1, 2, 3, 128), (99, 88, 77, 0)])
        payload = quantize(source)
        self.assertEqual(len(payload), 576)
        self.assertEqual(payload[:4], bytes(4))
        expected = bytearray(source)
        for at in range(0, len(expected), 4):
            if not expected[at + 3]:
                expected[at:at + 4] = bytes(4)
        self.assertEqual(color_atlas.rgba(payload), expected)
        first, second = payload[64] >> 4, payload[64] & 15
        self.assertEqual(payload[first * 4:first * 4 + 4], source[:4])
        self.assertEqual(payload[second * 4:second * 4 + 4], source[4:8])

    def test_adaptive_palette_is_deterministic_bounded_and_retains_opacity_classes(self):
        colors = [(x * 8, y * 8, (x + y) * 4, (x * 255 // 31) if y < 16 else 255)
                  for y in range(32) for x in range(32)]
        source = bytes(channel for color in colors for channel in color)
        payload = quantize(source)
        self.assertEqual(payload, quantize(source))
        decoded = color_atlas.rgba(payload)
        self.assertLessEqual(len({payload[at:at + 4] for at in range(4, 64, 4)} - {bytes(4)}), 15)
        self.assertTrue(any(0 < value < 255 for value in decoded[3::4]))
        for before, after in zip(source[3::4], decoded[3::4]):
            if before == 0:
                self.assertEqual(after, 0)
            elif before == 255:
                self.assertEqual(after, 255)
            else:
                self.assertTrue(0 < after < 255)

    def test_color_container_canonical_roundtrip_aliases_and_distinct_magic(self):
        red, green = (quantize(pixels([color])) for color in ((255, 0, 0, 255), (0, 255, 0, 128)))
        glyphs = {'😀': red, '❤️': green, '❤': green}
        data = color_atlas.encode(glyphs)
        self.assertEqual(color_atlas.decode(data), glyphs)
        self.assertEqual(data, color_atlas.encode(dict(reversed(list(glyphs.items())))))
        self.assertEqual(atlas.HEADER.unpack_from(data)[:5], (b'KEMC', 1, 32, 3, 32))
        self.assertEqual(len(data) - atlas.HEADER.unpack_from(data)[6], 2 * 576)
        with self.assertRaises(ValueError):
            atlas.decode(data)
        with self.assertRaises(ValueError):
            color_atlas.decode(atlas.encode({'😀': bytes([128]) + bytes(127)}))

    def test_palette_transparent_index_zero_and_unused_reference_are_strict(self):
        original = quantize(pixels([(255, 0, 0, 255)]))
        for change in ('index_zero', 'empty_reference', 'transparent_color'):
            payload = bytearray(original)
            if change == 'index_zero':
                payload[0] = 1
            elif change == 'empty_reference':
                payload[64] = 0xff
            else:
                payload[60] = 255
            with self.subTest(change=change), self.assertRaises(ValueError):
                color_atlas.validate_payload(bytes(payload))
        for payload in (original[:-1], original + b'\0', bytearray(original)):
            with self.assertRaises(ValueError):
                color_atlas.validate_payload(payload)

    def test_all_transparent_canonical_payload(self):
        payload = quantize(pixels([(255, 12, 8, 0)]))
        self.assertEqual(payload, bytes(576))
        self.assertEqual(color_atlas.decode(color_atlas.encode({'😀': payload})), {'😀': payload})

    def test_corrupted_header_offsets_payload_and_eight_mib_bound(self):
        original = color_atlas.encode({'😀': quantize(pixels([(255, 0, 0, 255)]))})
        for index, value in ((1, 2), (2, 64), (3, 8193), (4, 36), (5, 31), (6, 45), (7, len(original) - 1)):
            header = list(atlas.HEADER.unpack_from(original))
            header[index] = value
            with self.subTest(index=index), self.assertRaises(ValueError):
                color_atlas.decode(atlas.HEADER.pack(*header) + original[32:])
        with self.assertRaises(ValueError):
            color_atlas.decode(b'x' * (8 * 1024 * 1024 + 1))
        record = list(atlas.ENTRY.unpack_from(original, 32))
        record[1] += 4
        with self.assertRaises(ValueError):
            color_atlas.decode(original[:32] + atlas.ENTRY.pack(*record) + original[44:])

    def test_png_rgba_round_trip_and_contact_sheet(self):
        source = bytes((255, 0, 0, 255, 0, 255, 0, 128))
        data = png(2, 1, source)
        self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
        offset, scanlines = 8, bytearray()
        while offset < len(data):
            size = struct.unpack_from('>I', data, offset)[0]
            kind, content = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + size]
            if kind == b'IDAT':
                scanlines.extend(zlib.decompress(content))
            offset += 12 + size
        self.assertEqual(scanlines, b'\0' + source)
        glyph = quantize(pixels([(255, 40, 10, 255)]))
        self.assertEqual(contact_sheet({'😀': glyph}), contact_sheet({'😀': glyph}))

    def test_mono_encoding_stays_byte_identical(self):
        source = {'😀': bytes([128]) + bytes(127), '❤️': bytes([1]) * 128}
        encoded = atlas.encode(source)
        # Existing KEMJ layout: two keys at56/62, aligned pixels at68/196.
        expected = struct.pack('<4s7I', b'KEMJ', 1, 32, 2, 32, 56, 68, 324)
        expected += struct.pack('<IIHHIIHH', 56, 68, 6, 0, 62, 196, 4, 0)
        expected += '❤️😀'.encode() + bytes(2) + bytes([1]) * 128 + bytes([128]) + bytes(127)
        self.assertEqual(encoded, expected)
        self.assertEqual(atlas.decode(encoded), source)


if __name__ == '__main__':
    unittest.main()
