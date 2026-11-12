"""Bounded monochrome image preparation for the current PeriPage raster profile."""
from dataclasses import dataclass
from io import BytesIO
import math

from PIL import Image, ImageOps, UnidentifiedImageError

MAX_INPUT_BYTES = 8 * 1024 * 1024
MAX_PIXELS = 4 * 1024 * 1024
MAX_SIDE = 4096


class RasterError(ValueError):
    pass


@dataclass(frozen=True)
class Raster:
    data: bytes
    head_width: int
    width: int
    height: int
    preview: bytes = None

    @property
    def row_bytes(self):
        return self.head_width // 8

    def block(self, start, count):
        if not 0 <= start < self.height or not 1 <= count <= self.height - start:
            raise RasterError('invalid_block')
        return self.data[start * self.row_bytes:(start + count) * self.row_bytes]


def decode_rgb(png):
    if not isinstance(png, bytes) or not 1 <= len(png) <= MAX_INPUT_BYTES:
        raise RasterError('invalid_image_size')
    try:
        with Image.open(BytesIO(png)) as image:
            if image.format != 'PNG' or getattr(image, 'n_frames', 1) != 1:
                raise RasterError('unsupported_image')
            width, height = image.size
            if not 1 <= width <= MAX_SIDE or not 1 <= height <= MAX_SIDE or width * height > MAX_PIXELS:
                raise RasterError('invalid_image_dimensions')
            image.load()
            image = ImageOps.exif_transpose(image).convert('RGBA')
            background = Image.new('RGBA', image.size, (255, 255, 255, 255))
            background.alpha_composite(image)
            return background.convert('RGB')
    except (UnidentifiedImageError, OSError, SyntaxError, Image.DecompressionBombError) as error:
        raise RasterError('invalid_image') from error


def _decode(png):
    return decode_rgb(png).convert('L')


def prepare(png, *, head_width, width_dots, height_limit, dpi, feed_dpi):
    """Fit without crop, preserve physical aspect, and pack full-width FS raster rows."""
    values = (head_width, width_dots, height_limit, dpi, feed_dpi)
    if any(type(value) is not int for value in values):
        raise RasterError('invalid_geometry')
    if not 8 <= head_width <= 2048 or head_width % 8 or not 1 <= width_dots <= head_width:
        raise RasterError('invalid_geometry')
    if not 1 <= height_limit <= 3543 or not 72 <= dpi <= 600 or not 72 <= feed_dpi <= 600:
        raise RasterError('invalid_geometry')
    image = _decode(png)
    source_width, source_height = image.size
    width = width_dots
    height = max(1, math.floor(width * source_height * feed_dpi / (source_width * dpi) + .5))
    if height > height_limit:
        width = math.floor(height_limit * source_width * dpi / (source_height * feed_dpi))
        if width < 1:
            raise RasterError('content_does_not_fit')
        height = min(height_limit, max(1, math.floor(width * source_height * feed_dpi / (source_width * dpi) + .5)))
    scaled = image.resize((width, height), Image.Resampling.LANCZOS)
    mono = scaled.convert('1', dither=Image.Dither.FLOYDSTEINBERG)
    canvas = Image.new('1', (head_width, height), 1)
    canvas.paste(mono, ((head_width - width) // 2, 0))
    data = bytes(value ^ 0xff for value in canvas.tobytes())
    if len(data) != head_width // 8 * height:
        raise RasterError('invalid_raster')
    return Raster(data=data, head_width=head_width, width=width, height=height)
