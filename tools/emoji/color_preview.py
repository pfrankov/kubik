"""Small deterministic RGBA PNG contact sheet using only the Python standard library."""
import struct
import zlib

from color_atlas import rgba

CHOICES = ('😀', '❤️', '👍🏽', '👩‍💻', '🇷🇺', '👨‍👩‍👧‍👦', '🎉', '🖨️')


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def png(width, height, pixels):
    if not 1 <= width <= 4096 or not 1 <= height <= 4096 or len(pixels) != width * height * 4:
        raise ValueError('Invalid bounded RGBA PNG dimensions')
    stride = width * 4
    scanlines = b''.join(b'\0' + pixels[y * stride:(y + 1) * stride] for y in range(height))
    header = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(scanlines, 6)) + chunk(b'IEND', b'')


def draw_glyph(pixels, width, glyph, ox, oy, scale, background):
    for y in range(32):
        for x in range(32):
            at = (y * 32 + x) * 4
            red, green, blue, alpha = glyph[at:at + 4]
            colors = [((value * alpha + background[axis] * (255 - alpha) + 127) // 255)
                      for axis, value in enumerate((red, green, blue))]
            row = bytes([*colors, 255]) * scale
            for dy in range(scale):
                target = ((oy + y * scale + dy) * width + ox + x * scale) * 4
                pixels[target:target + len(row)] = row


def contact_sheet(glyphs):
    scale, cell = 5, 176
    width, height = cell * 4, cell * 4
    pixels = bytearray(width * height * 4)
    for half, background in enumerate(((255, 255, 255), (16, 23, 25))):
        for y in range(half * cell * 2, (half + 1) * cell * 2):
            first = y * width * 4
            pixels[first:first + width * 4] = bytes([*background, 255]) * width
        for number, sequence in enumerate(CHOICES):
            if sequence not in glyphs:
                continue
            ox, oy = (number % 4) * cell + 8, (half * 2 + number // 4) * cell + 8
            draw_glyph(pixels, width, rgba(glyphs[sequence]), ox, oy, scale, background)
    return png(width, height, pixels)
