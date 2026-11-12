"""Lay out a bounded article in physical coordinates for the PeriPage raster stream."""
import base64
import math
from pathlib import Path
import re

from PIL import Image, ImageDraw, ImageFont
from .printer_emoji import CLUSTERS, EMOJI, EmojiError
from .printer_image_raster import Raster, RasterError, _decode

FONT_PATH = Path(__file__).with_name('PT_Sans-Web-Regular.ttf')
MAX_PAGES = 4


def typeface(font_path, mm, dpi):
    target = max(1, round(mm * dpi / 25.4))
    font = ImageFont.truetype(str(font_path), max(1, target))
    bbox = font.getbbox('H')
    cap = bbox[3] - bbox[1]
    if not cap:
        raise RasterError('invalid_font')
    return ImageFont.truetype(str(font_path), max(1, round(target * target / cap)))


def emoji_images(atlas, text):
    try:
        return {glyph['sequence']: Image.frombytes('1', (32, 32), base64.b64decode(glyph['bitmap'], validate=True))
                for glyph in atlas.glyphs(text)}
    except (EmojiError, ValueError) as error:
        raise RasterError(getattr(error, 'code', 'unsupported_character')) from None


class TextLayout:
    def __init__(self, draw, font, width, emojis):
        self.draw, self.font, self.width, self.emojis = draw, font, width, emojis
        self.x = self.y = 0
        self.breaks = []
        self.cap = font.getbbox('H')[3] - font.getbbox('H')[1]
        self.line = max(1, math.ceil(sum(font.getmetrics()) * 1.08))
        self.missing = self.fingerprint('\U0010ffff')
        self.widths = {}

    def fingerprint(self, text):
        mask = self.font.getmask(text, mode='L')
        return mask.size, bytes(mask)

    def advance(self, cluster):
        if cluster not in self.widths:
            if cluster in self.emojis:
                self.widths[cluster] = self.cap * 1.1
            elif not cluster.isspace() and self.fingerprint(cluster) == self.missing:
                raise RasterError('unsupported_character')
            else:
                self.widths[cluster] = self.font.getlength(cluster)
        return self.widths[cluster]

    def measure(self, word):
        return sum(self.advance(c) for c in CLUSTERS.findall(word))

    def newline(self):
        self.x = 0
        self.y += self.line
        self.breaks.append(self.y)

    def word(self, text, base):
        width = self.measure(text)
        if self.x and self.x + width > self.width:
            self.newline()
        for cluster in CLUSTERS.findall(text):
            advance = self.advance(cluster)
            if advance > self.width:
                raise RasterError('content_does_not_fit')
            if self.x and self.x + advance > self.width:
                self.newline()
            self.draw_cluster(cluster, base)
            self.x += advance

    def draw_cluster(self, cluster, base):
        position = (round(self.x), base + self.y)
        if cluster not in self.emojis:
            self.draw.text(position, cluster, fill=0, font=self.font, anchor='lt')
            return
        edge = max(1, self.cap)
        mask = self.emojis[cluster].resize((edge, edge), Image.Resampling.NEAREST)
        self.draw.bitmap(position, mask, fill=0)

    def render(self, text, base):
        for token in re.findall(r'\n|[^\S\n]+|[^\s]+', text.replace('\t', '    ')):
            if token == '\n':
                self.newline()
            elif token.isspace():
                if self.x:
                    self.x += self.measure(token)
            else:
                self.word(token, base)
        return base + self.y + self.line, [base + value for value in self.breaks]


def image_geometry(image, width, dpi, feed_dpi):
    physical_height = max(1, round(width * image.height / image.width))
    rows = max(1, round(physical_height * feed_dpi / dpi))
    return physical_height, rows


def layout_blocks(blocks, images, atlas, inner, limit, page_height, margin, dpi, feed_dpi, font_path, default_font_mm):
    ink = ImageDraw.Draw(inner)
    text = '\n'.join(block['text'] for block in blocks if block['type'] == 'text')
    emojis = emoji_images(atlas, text)
    placements, boundaries, decoded, fonts, y = [], [], {}, {}, margin
    try:
        for block in blocks:
            if block['type'] == 'text':
                size = block.get('font_mm', default_font_mm)
                if size not in fonts:
                    fonts[size] = typeface(font_path, size, dpi)
                font = fonts[size]
                y, breaks = TextLayout(ink, font, inner.width, emojis).render(block['text'], y)
                boundaries.extend(breaks)
            else:
                ref = block['image_ref']
                if ref not in decoded:
                    decoded[ref] = _decode(images[ref])
                image = decoded[ref]
                width = round(block.get('width_mm', inner.width * 25.4 / dpi) * dpi / 25.4)
                if not 1 <= width <= inner.width:
                    raise RasterError('content_does_not_fit')
                virtual_height, rows = image_geometry(image, width, dpi, feed_dpi)
                if virtual_height > page_height - 2 * margin:
                    raise RasterError('content_does_not_fit')
                placements.append((image, width, rows, y, y + virtual_height))
                y += virtual_height
            y += margin
            boundaries.append(y)
            if y > limit:
                raise RasterError('content_does_not_fit')
    except (OSError, KeyError, ValueError) as error:
        if isinstance(error, RasterError):
            raise
        raise RasterError('invalid_document') from error
    return y, placements, boundaries


def paste_images(mono, placements, dpi, feed_dpi):
    for image, width, rows, top, _bottom in placements:
        picture = image.resize((width, rows), Image.Resampling.LANCZOS).convert('1', dither=Image.Dither.FLOYDSTEINBERG)
        row = round(top * feed_dpi / dpi)
        if row + rows > mono.height:
            raise RasterError('content_does_not_fit')
        mono.paste(picture, ((mono.width - width) // 2, row))


def page_ranges(height, boundaries, page_height, margin):
    ranges, start = [], 0
    while start < height:
        limit = start + page_height - 2 * margin
        if height <= limit:
            end = height
        else:
            candidates = [point for point in boundaries if start < point <= limit]
            if not candidates:
                raise RasterError('content_does_not_fit')
            end = max(candidates)
        ranges.append((start, end))
        if len(ranges) > MAX_PAGES:
            raise RasterError('content_does_not_fit')
        start = end
    return ranges


def render_page(inner, placements, start, end, margin, head_width, width_dots, dpi, feed_dpi):
    virtual_height = end - start + 2 * margin
    canvas = Image.new('L', (width_dots, virtual_height), 255)
    canvas.paste(inner.crop((0, start, inner.width, end)), (margin, margin))
    height = max(1, math.ceil(virtual_height * feed_dpi / dpi))
    scaled = canvas.resize((width_dots, height), Image.Resampling.LANCZOS)
    mono = scaled.point(lambda value: 255 if value >= 128 else 0, mode='1')
    local = [(image, width, rows, top - start + margin, bottom - start + margin)
             for image, width, rows, top, bottom in placements if start <= top and bottom <= end]
    paste_images(mono, local, dpi, feed_dpi)
    result = Image.new('1', (head_width, height), 1)
    result.paste(mono, ((head_width - width_dots) // 2, 0))
    return Raster(bytes(byte ^ 0xff for byte in result.tobytes()), head_width, width_dots, height)


def compose(blocks, images, atlas, *, head_width, width_dots, height_limit, dpi, feed_dpi,
            default_font_mm=2.5, font_path=FONT_PATH):
    if head_width % 8 or not 1 <= width_dots <= head_width or not 1 <= height_limit <= 3543:
        raise RasterError('invalid_geometry')
    if not 72 <= dpi <= 600 or not 72 <= feed_dpi <= 600:
        raise RasterError('invalid_geometry')
    page_height = math.floor(height_limit * dpi / feed_dpi)
    margin = max(1, round(dpi / 25.4))
    inner_width = width_dots - margin * 2
    if inner_width < 1 or page_height <= 2 * margin:
        raise RasterError('content_does_not_fit')
    virtual_limit = page_height * MAX_PAGES
    inner = Image.new('L', (inner_width, virtual_limit), 255)
    y, placements, boundaries = layout_blocks(blocks, images, atlas, inner, virtual_limit, page_height,
                                               margin, dpi, feed_dpi, font_path, default_font_mm)
    ranges = page_ranges(y, boundaries, page_height, margin)
    for _image, _width, _rows, top, bottom in placements:
        if sum(start <= top and bottom <= end for start, end in ranges) != 1:
            raise RasterError('content_does_not_fit')
    return [render_page(inner, placements, start, end, margin, head_width, width_dots, dpi, feed_dpi)
            for start, end in ranges]
