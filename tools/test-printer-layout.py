#!/usr/bin/env python3
"""Check physical-size text/QR layout through the production row renderer."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from contextlib import nullcontext
from frame_diff import write_png

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ('printer_layout', 'printer_layout_text', 'printer_layout_qr', 'printer_emoji', 'emoji_cluster')
QR = ROOT / 'firmware/managed_components/espressif__qrcode'
CC = os.environ.get('CC', 'cc')
FLAGS = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Ifirmware/main', f'-I{QR}']


def png(pbm):
    header, density, dimensions, data = pbm.read_bytes().split(b'\n', 3)
    assert header == b'P4' and density.startswith(b'# dpi ')
    dpi, feed_dpi = map(int, density.split()[2:])
    width, height = map(int, dimensions.split())
    assert len(data) == width // 8 * height
    output_height = max(1, round(height * dpi / feed_dpi))
    rgb = bytearray()
    for y in range(output_height):
        source_y = min(height - 1, (2 * y + 1) * height // (2 * output_height))
        for value in data[source_y * (width // 8):(source_y + 1) * (width // 8)]:
            for bit in range(8):
                rgb.extend(bytes((0 if value & (0x80 >> bit) else 255,)) * 3)
    output = pbm.with_suffix('.png')
    write_png(output, width, output_height, rgb)
    return output


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence', type=Path, help='Create a NEW directory with actual rows and physical-aspect previews')
args = parser.parse_args()
if args.evidence:
    args.evidence = args.evidence.expanduser().resolve()
    args.evidence.mkdir(parents=True, exist_ok=False)
context = nullcontext(str(args.evidence)) if args.evidence else tempfile.TemporaryDirectory(prefix='kubik-printer-layout-')
with context as temporary:
    folder = Path(temporary)
    outputs = []
    for target in ('ESP32C6', 'ESP32S3'):
        target_dir = folder / target
        target_dir.mkdir()
        objects = []
        for source in SOURCES:
            obj = target_dir / f'{source}.o'
            subprocess.run([CC, *FLAGS, f'-DCONFIG_IDF_TARGET_{target}=1',
                            '-Dmalloc=printer_test_malloc', '-Dcalloc=printer_test_calloc', '-Dfree=printer_test_free',
                            '-c', f'firmware/main/{source}.c', '-o', str(obj)], cwd=ROOT, check=True)
            objects.append(str(obj))
        binary = target_dir / 'layout'
        subprocess.run([CC, *FLAGS, 'firmware/sim/printer_layout_test.c', 'firmware/main/font.c',
                        'firmware/main/font_data.c', str(QR / 'qrcodegen.c'), *objects, '-lm', '-o', str(binary)],
                       cwd=ROOT, check=True)
        subprocess.run([str(binary), str(target_dir)], cwd=ROOT, check=True, timeout=30)
        outputs.append({p.name: p.read_bytes() for p in target_dir.glob('*.pbm')})
    assert outputs[0] == outputs[1], 'The same printer document differs between Cube profiles'
    if sys.platform == 'darwin':
        images = [png(p) for p in sorted((folder / 'ESP32C6').glob('qr-*.pbm'))]
        for p in (folder / 'ESP32C6').glob('text-*.pbm'): png(p)
        decoded = json.loads(subprocess.check_output(['swift', 'tools/fixtures/qr-decode.swift', *map(str, images)], cwd=ROOT, text=True))
        assert decoded and all(text == 'https://kubik.local/printer?size=20mm' for text in decoded.values())
        print('Vision SDK: native 1-bit rows at their declared physical aspect decode to the exact payload')
print('Printer layout: both Cube targets passed ASan/UBSan; no full-page allocation')
