#!/usr/bin/env python3
"""Verify direct S3 font/icon/QR/edge compositing with sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='kubik-native-overlay-') as temporary:
    executable = str(Path(temporary) / 'overlay-test')
    sources = ['sim/native_overlay_test', 'main/render_text', 'main/render_overlay_s3', 'main/render_scene', 'main/render_projection', 'main/font', 'main/font_data']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                    '-DCONFIG_IDF_TARGET_ESP32S3=1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    *[str(ROOT / f'firmware/{source}.c') for source in sources], '-lm', '-o', executable], check=True)
    subprocess.run([executable], check=True)
