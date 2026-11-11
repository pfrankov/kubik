#!/usr/bin/env python3
"""Generate a private monochrome Apple emoji atlas from this Mac's system font."""
import argparse
import base64
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from urllib.request import urlopen

from atlas import decode, encode, GLYPH_BYTES, pinned_sequences, UNICODE_SHA256, UNICODE_URL

ROOT = Path(__file__).resolve().parents[2]
FONT = Path('/System/Library/Fonts/Apple Color Emoji.ttc')
RENDERER = Path(__file__).with_name('render-apple.swift')


def private_destination(path):
    path = path.expanduser()
    if path.is_symlink():
        raise ValueError('Output must not be a symlink')
    path = path.resolve()
    if path == ROOT or ROOT in path.parents:
        raise ValueError('Generated Apple artwork must stay outside the source repository')
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError('Output must be a regular private file')
    return path


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def unicode_data(path):
    if path is not None:
        with path.open('rb') as stream:
            data = stream.read(1024 * 1024 + 1)
    else:
        with urlopen(UNICODE_URL, timeout=20) as response:
            data = response.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise ValueError('Unicode emoji-test exceeds its one MiB bound')
    return pinned_sequences(data)


def checked_font(result):
    if result.get('postscript') != 'AppleColorEmoji' or result.get('edge') != 32:
        raise ValueError('Renderer used an unexpected font or glyph size')
    if Path(result.get('fontURL', '')).resolve() != FONT:
        raise ValueError('Renderer did not use this Mac system Apple Color Emoji file')


def checked_render(result, requests):
    checked_font(result)
    expected, seen, glyphs = set(requests), set(), {}
    for row in result['glyphs']:
        value = row['sequence']
        if value not in expected or value in seen:
            raise ValueError('Renderer returned an unknown or duplicate sequence')
        bitmap = base64.b64decode(row['bitmap'], validate=True)
        if len(bitmap) != GLYPH_BYTES or not any(bitmap):
            raise ValueError('Renderer returned an empty or malformed glyph')
        glyphs[value] = bitmap
        seen.add(value)
    for row in result['unsupported']:
        value = row['sequence']
        if value not in expected or value in seen or not row.get('reason'):
            raise ValueError('Renderer returned an invalid unsupported sequence')
        seen.add(value)
    if seen != expected:
        raise ValueError('Renderer did not account for every requested sequence')
    return glyphs


def private_write(path, data):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.apple-emoji-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            os.fchmod(output.fileno(), 0o600)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def render(requests, preview, color=False):
    with tempfile.TemporaryDirectory(prefix='kubik-apple-emoji-') as directory:
        work = Path(directory)
        source, destination = work / 'sequences.json', work / 'rendered.json'
        source.write_text(json.dumps(requests, ensure_ascii=False))
        command = ['swift', str(RENDERER), str(source), str(destination), str(preview)]
        subprocess.run(command + (['--color'] if color else []),
                       check=True, timeout=180, capture_output=True, text=True)
        return json.loads(destination.read_text())


def provenance(result, requests, glyphs, atlas, format_name, bitmap):
    checks = {sequence: sequence in glyphs for sequence in ('😀', '❤️', '👍🏽', '👩‍💻', '🇷🇺', '👨‍👩‍👧‍👦')}
    return {
        'format': format_name, 'edge': 32, 'bitmap': bitmap,
        'created_utc': datetime.now(timezone.utc).isoformat(),
        'unicode': {'version': '17.0', 'url': UNICODE_URL, 'sha256': UNICODE_SHA256},
        'font': {'postscript': result['postscript'], 'version': result['version'],
                 'system_path': str(FONT), 'sha256': file_hash(FONT)},
        'atlas_sha256': hashlib.sha256(atlas).hexdigest(), 'atlas_bytes': len(atlas),
        'requested_sequences': len(requests), 'rendered_sequences': len(glyphs),
        'unsupported_reasons': dict(Counter(row['reason'] for row in result['unsupported'])),
        'unsupported': result['unsupported'], 'checks': checks,
        'artwork_scope': 'Private system-font rasterization; artwork and font are not bundled with source',
    }


def build(source, output, preview):
    if sys.platform != 'darwin' or not FONT.is_file():
        raise ValueError('Run on a Mac with its installed Apple Color Emoji font')
    output, preview = private_destination(output), private_destination(preview)
    requests = unicode_data(source)
    preview.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    result = render(requests, preview)
    glyphs = checked_render(result, requests)
    atlas = encode(glyphs)
    if decode(atlas) != glyphs:
        raise ValueError('Generated atlas failed its complete round trip')
    private_write(output, atlas)
    preview.chmod(0o600)
    record = provenance(result, requests, glyphs, atlas, 'KEMJ v1', 'MSB-first 1-bit black on white')
    private_write(output.with_suffix('.source.json'), json.dumps(record, ensure_ascii=False, indent=2).encode())
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unicode-source', type=Path, help='Cached official emoji-test-17.0.txt; SHA-256 is mandatory')
    parser.add_argument('--output', type=Path, required=True, help='Private atlas destination outside this repository')
    parser.add_argument('--preview', type=Path, help='Private contact sheet destination')
    args = parser.parse_args()
    preview = args.preview or args.output.with_name(args.output.stem + '-preview.png')
    try:
        result = build(args.unicode_source, args.output, preview)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Apple emoji atlas: {error}\n')
    print(json.dumps({key: result[key] for key in ('rendered_sequences', 'atlas_bytes', 'atlas_sha256', 'checks')},
                     ensure_ascii=False))


if __name__ == '__main__':
    main()
