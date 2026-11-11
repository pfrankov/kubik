#!/usr/bin/env python3
"""Check physical-size text/QR layout through the production row renderer."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from frame_diff import write_png

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ('printer_layout', 'printer_layout_text', 'printer_layout_qr')
QR = ROOT / 'firmware/managed_components/espressif__qrcode'
CC = os.environ.get('CC', 'cc')
FLAGS = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Ifirmware/main', f'-I{QR}']


def png(pbm):
    header, dimensions, data = pbm.read_bytes().split(b'\n', 2)
    assert header == b'P4'
    width, height = map(int, dimensions.split())
    assert len(data) == width // 8 * height
    rgb = bytearray()
    for value in data:
        for bit in range(8):
            rgb.extend(bytes((0 if value & (0x80 >> bit) else 255,)) * 3)
    output = pbm.with_suffix('.png')
    write_png(output, width, height, rgb)
    return output


with tempfile.TemporaryDirectory(prefix='kubik-printer-layout-') as temporary:
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
        images = [png(p) for p in sorted((folder / 'ESP32C6').glob('*.pbm'))]
        decoded = json.loads(subprocess.check_output(['swift', 'tools/fixtures/qr-decode.swift', *map(str, images)], cwd=ROOT, text=True))
        assert decoded and all(text == 'https://kubik.local/printer?size=20mm' for text in decoded.values())
        print('Vision SDK: both unmodified 1-bit printer QR layouts decode to the exact payload')
print('Printer layout: both Cube targets passed ASan/UBSan; no full-page allocation')
