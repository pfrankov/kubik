#!/usr/bin/env python3
"""Generate a private adaptive-palette color atlas from this Mac's Apple emoji."""
import argparse
import base64
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from color_atlas import decode, encode
from color_preview import contact_sheet
from color_quant import quantize

spec = importlib.util.spec_from_file_location('apple_source', Path(__file__).with_name('build-apple.py'))
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


def checked_colors(result, requests):
    source.checked_render(result, requests)
    cache, glyphs = {}, {}
    for row in result['glyphs']:
        if not isinstance(row.get('rgba'), str):
            raise ValueError('Renderer did not provide straight RGBA')
        pixels = base64.b64decode(row['rgba'], validate=True)
        if len(pixels) != 32 * 32 * 4:
            raise ValueError('Renderer returned malformed straight RGBA')
        if pixels not in cache:
            cache[pixels] = quantize(pixels)
        glyphs[row['sequence']] = cache[pixels]
    return glyphs


def build(unicode_source, output, preview):
    if sys.platform != 'darwin' or not source.FONT.is_file():
        raise ValueError('Run on a Mac with its installed Apple Color Emoji font')
    output, preview = source.private_destination(output), source.private_destination(preview)
    requests = source.unicode_data(unicode_source)
    with tempfile.TemporaryDirectory(prefix='kubik-apple-color-') as directory:
        result = source.render(requests, Path(directory) / 'mono-preview.png', color=True)
    glyphs = checked_colors(result, requests)
    atlas = encode(glyphs)
    if decode(atlas) != glyphs:
        raise ValueError('Color atlas failed its complete round trip')
    source.private_write(output, atlas)
    source.private_write(preview, contact_sheet(glyphs))
    record = source.provenance(result, requests, glyphs, atlas, 'KEMC v1',
                               '15 adaptive straight-RGBA colors plus transparent; packed 4-bit MSB first')
    record['quantizer'] = 'deterministic alpha-aware weighted median cut, at most 15 colors'
    source.private_write(output.with_suffix('.source.json'), json.dumps(record, ensure_ascii=False, indent=2).encode())
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unicode-source', type=Path, help='Pinned cached official Unicode17 emoji-test')
    parser.add_argument('--output', required=True, type=Path, help='Private KEMC destination outside the repository')
    parser.add_argument('--preview', type=Path, help='Private color contact sheet destination')
    args = parser.parse_args()
    preview = args.preview or args.output.with_name(args.output.stem + '-preview.png')
    try:
        result = build(args.unicode_source, args.output, preview)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Apple color emoji atlas: {error}\n')
    print(json.dumps({key: result[key] for key in ('rendered_sequences', 'atlas_bytes', 'atlas_sha256', 'checks')},
                     ensure_ascii=False))


if __name__ == '__main__':
    main()
