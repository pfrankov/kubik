"""Bounded color preview for the image sent to the printer."""
from dataclasses import replace
from PIL import Image

from .printer_image_raster import decode_rgb, prepare

SIZE = 128
PAYLOAD_BYTES = 32 + SIZE * SIZE // 2


def prepare_with_preview(png, **geometry):
    raster = prepare(png, **geometry)
    try:
        return replace(raster, preview=make_preview(png))
    except (MemoryError, OSError):
        return raster


def make_preview(png):
    image = decode_rgb(png)
    image.thumbnail((SIZE, SIZE), Image.Resampling.LANCZOS)
    canvas = Image.new('RGB', (SIZE, SIZE), (0, 0, 0))
    canvas.paste(image, ((SIZE - image.width) // 2, (SIZE - image.height) // 2))
    indexed = canvas.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    palette = (indexed.getpalette() or [])[:48]
    palette += [0] * (48 - len(palette))
    colors = bytearray()
    for index in range(0, 48, 3):
        red, green, blue = palette[index:index + 3]
        colors.extend((((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)).to_bytes(2, 'big'))
    pixels = indexed.tobytes()
    packed = bytes((pixels[i] << 4) | pixels[i + 1] for i in range(0, len(pixels), 2))
    preview = bytes(colors) + packed
    assert len(preview) == PAYLOAD_BYTES
    return preview
