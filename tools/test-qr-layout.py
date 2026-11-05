#!/usr/bin/env python3
"""Run actual setup QR layout/render checks on C6/S3; optionally export native S3 PNGs."""
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from frame_diff import write_png

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path)
args = parser.parse_args()
if args.out:
    args.out.mkdir(parents=True, exist_ok=True)
sources = runpy.run_path(str(ROOT / 'tools/test-render.py'))['RENDER']
with tempfile.TemporaryDirectory(prefix='kubik-qr-layout-') as temporary:
    for target in ('esp32c6', 'esp32s3'):
        executable = str(Path(temporary) / target)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        f'-DCONFIG_IDF_TARGET_ESP32S3={int(target == "esp32s3")}',
                        '-Ifirmware/managed_components/espressif__qrcode',
                        'firmware/sim/qr_layout_test.c', 'firmware/main/face_setup.c', 'firmware/main/qr.c',
                        'firmware/managed_components/espressif__qrcode/qrcodegen.c',
                        *[f'firmware/main/{source}.c' for source in sources], '-lm', '-o', executable], cwd=ROOT, check=True)
        subprocess.run([executable, *([str(args.out.resolve())] if args.out else [])], cwd=ROOT, check=True)
if args.out:
    for raw in args.out.glob('*.rgb'):
        data = raw.read_bytes()
        assert len(data) == 368 * 448 * 3
        write_png(raw.with_suffix('.png'), 368, 448, data)
        raw.unlink()
    print(f'Native QR PNGs: {args.out.resolve()}')
    if sys.platform == 'darwin':
        images = sorted(args.out.glob('*.png'))
        decoded = json.loads(subprocess.check_output(['swift', str(ROOT / 'tools/fixtures/qr-decode.swift'), *map(str, images)], text=True))
        for image in images:
            expected = 'WIFI:T:WPA;S:Kubik-4F2A;P:kubik4f2abcd;;' if image.name.startswith('wifi-') else 'KUBIK-QR-' + image.name.split('-')[1]
            assert decoded[image.name] == expected, (image, decoded[image.name])
        (args.out / 'decoded.json').write_text(json.dumps(decoded, indent=2) + '\n')
        print(f'Vision SDK: all {len(images)} unmodified native PNGs decode to their exact source payloads')
