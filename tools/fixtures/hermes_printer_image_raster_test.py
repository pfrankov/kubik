"""Generated images are bounded, physically scaled and rasterized without side effects."""
from io import BytesIO
import unittest

from PIL import Image
import hermes_test_support  # Registers the local hermes_kubik package.
from hermes_kubik.printer_image_raster import RasterError, prepare


def png(image):
    output = BytesIO()
    image.save(output, format='PNG')
    return output.getvalue()


def raster(image, *, limit=1328, width=576):
    return prepare(png(image), head_width=576, width_dots=width,
                   height_limit=limit, dpi=304, feed_dpi=225)


class PrinterImageRaster(unittest.TestCase):
    def test_physical_aspect_and_uncropped_fit(self):
        square = raster(Image.new('L', (100, 100), 0))
        self.assertEqual((square.width, square.height, square.row_bytes), (576, 426, 72))
        self.assertEqual(len(square.data), 426 * 72)
        self.assertTrue(all(value == 255 for value in square.data))
        portrait = raster(Image.new('L', (100, 200), 0), limit=200)
        self.assertEqual((portrait.width, portrait.height), (135, 200))
        self.assertEqual(portrait.data[0], 0)  # The centered image does not mark the edge.
        self.assertEqual(portrait.data[35], 255)
        self.assertEqual(portrait.data[71], 0)

    def test_alpha_and_monochrome_are_explicit(self):
        image = Image.new('RGBA', (32, 32), (0, 0, 0, 0))
        white = raster(image, width=32)
        self.assertFalse(any(white.data))
        image = Image.new('RGBA', (32, 32), (0, 0, 0, 255))
        black = raster(image, width=32)
        self.assertTrue(any(black.data))
        self.assertEqual(black.block(0, 1), black.data[:72])
        with self.assertRaises(RasterError):
            black.block(black.height, 1)
        with self.assertRaises(RasterError):
            black.block(0, black.height + 1)

    def test_invalid_and_oversized_input_never_reaches_rendering(self):
        with self.assertRaises(RasterError):
            prepare(b'not a PNG', head_width=576, width_dots=576, height_limit=1328, dpi=304, feed_dpi=225)
        with self.assertRaises(RasterError):
            raster(Image.new('1', (2050, 2050), 1))
        with self.assertRaises(RasterError):
            prepare(b'x', head_width=575, width_dots=575, height_limit=1, dpi=304, feed_dpi=225)


if __name__ == '__main__':
    unittest.main()
