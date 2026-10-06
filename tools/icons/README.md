# Firmware Lucide icons

The firmware icons are derived from the Lucide SVG files in `sources/`, pinned
to revision `f12b0de177fbc2a6795e99be065887e72b237123` (Lucide 0.468.0). The
source set and pin are recorded in `sources/PROVENANCE.json`; the Lucide ISC
license is included at `../../licenses/ISC-Lucide.txt`.

`tools/icons/build.py` rasterizes each SVG at 2x and downsamples it to a
48-by-48 4-bit alpha mask. The resulting C includes are checked in under
`firmware/main/`, so firmware builds need no SVG renderer. To regenerate or
check them with the pinned generator dependencies:

```sh
uv run --with resvg-py==0.5.0 --with pillow==12.3.0 python tools/icons/build.py
uv run --with resvg-py==0.5.0 --with pillow==12.3.0 python tools/icons/build.py --check
```

Firmware colors each mask at composition time. Icons share the full-resolution
text overlay, which keeps their antialiased strokes crisp after the native
240-by-240 canvas is expanded to the panel's 480-by-480 output.

Each packed byte stores the left pixel in its high nibble and the right pixel
in its low nibble. The renderer uses that same order; `icons_test.c` checks
unscaled pixels against the source mask as well as clipping and RGB565 byte order.
