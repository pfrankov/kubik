"""Physical geometry, order, legibility boundaries and no partial document clipping."""
import io
from pathlib import Path
import tempfile
import unittest

from PIL import Image
from hermes_emoji_support import atlas
from hermes_kubik.printer_document import compose
from hermes_kubik.printer_emoji import EmojiAtlas
from hermes_kubik.printer_image_raster import RasterError

FONT = Path(__file__).resolve().parents[2] / 'tools/font/sources/PT_Sans-Web-Regular.ttf'


def png():
    image = Image.new('RGB', (64, 64), 'black')
    data = io.BytesIO(); image.save(data, format='PNG')
    return data.getvalue()


class DocumentLayout(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        path = Path(self.temp.name) / 'apple-emoji.bin'; path.write_bytes(atlas())
        self.atlas = EmojiAtlas(path)
        self.settings = dict(head_width=576, width_dots=480, height_limit=1328, dpi=304, feed_dpi=225,
                             font_path=FONT)

    def test_text_image_text_is_one_ordered_raster_with_physical_square_image(self):
        ref = 'a' * 32
        blocks = [{'type': 'text', 'text': 'Заголовок\nТекст перед картинкой.'},
                  {'type': 'image', 'image_ref': ref, 'width_mm': 20},
                  {'type': 'text', 'text': 'Текст после картинки.'}]
        raster, = compose(blocks, {ref: png()}, self.atlas, **self.settings)
        self.assertEqual(len(raster.data), raster.height * 72)
        rows = [sum(byte.bit_count() for byte in raster.data[y * 72:(y + 1) * 72]) for y in range(raster.height)]
        spans = []
        for y, count in enumerate(rows):
            if count >= 230:
                if spans and spans[-1][-1] == y - 1:
                    spans[-1].append(y)
                else:
                    spans.append([y])
        image_rows = max(spans, key=len)
        self.assertEqual(len(image_rows), round(239 * 225 / 304))
        self.assertTrue(any(rows[y] for y in range(image_rows[0])))
        self.assertTrue(any(rows[y] for y in range(image_rows[-1] + 1, len(rows))))
        self.assertLess(abs(239 * 25.4 / 304 - len(image_rows) * 25.4 / 225), .2)

    def test_long_article_splits_at_line_boundaries_without_losing_ink(self):
        blocks = [{'type': 'text', 'text': ('Текст для длинной статьи с переносами. ' * 30)}]
        pages = compose(blocks, {}, self.atlas, **self.settings)
        self.assertGreater(len(pages), 1)
        self.assertLessEqual(len(pages), 4)
        self.assertTrue(all(1 <= page.height <= 1328 and any(page.data) for page in pages))

    def test_page_break_never_splits_or_drops_an_image(self):
        ref = 'b' * 32
        blocks = [{'type': 'text', 'text': 'Начало статьи. ' * 30},
                  {'type': 'image', 'image_ref': ref, 'width_mm': 20},
                  {'type': 'text', 'text': 'Конец статьи.'}]
        pages = compose(blocks, {ref: png()}, self.atlas, **self.settings)
        counts = []
        for page in pages:
            rows = [sum(byte.bit_count() for byte in page.data[y * 72:(y + 1) * 72]) for y in range(page.height)]
            longest = run = 0
            for count in rows:
                run = run + 1 if count >= 230 else 0
                longest = max(longest, run)
            counts.append(longest)
        self.assertEqual(sum(value == round(239 * 225 / 304) for value in counts), 1)
        self.assertTrue(all(value < round(239 * 225 / 304) for value in counts if value != max(counts)))

    def test_long_content_and_requested_image_too_wide_fail_without_clipping(self):
        with self.assertRaisesRegex(RasterError, 'content_does_not_fit'):
            compose([{'type': 'text', 'text': 'Длинный текст ' * 1500}], {}, self.atlas, **self.settings)
        with self.assertRaisesRegex(RasterError, 'content_does_not_fit'):
            compose([{'type': 'image', 'image_ref': 'x', 'width_mm': 100}], {'x': png()}, self.atlas, **self.settings)

    def test_apple_emoji_is_whole_and_unknown_character_is_rejected(self):
        raster, = compose([{'type': 'text', 'text': 'Привет 😀'}], {}, self.atlas, **self.settings)
        self.assertTrue(any(raster.data))
        with self.assertRaisesRegex(RasterError, 'unsupported_character'):
            compose([{'type': 'text', 'text': 'A \U0010ffff'}], {}, self.atlas, **self.settings)


if __name__ == '__main__':
    unittest.main()
