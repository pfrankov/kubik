#!/usr/bin/env python3
"""Check mixed native color emoji rendering, scene ownership and dirty signatures."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['render_scene', 'render_text', 'render_overlay_s3', 'render_projection', 'render_emoji', 'emoji_cluster', 'font', 'font_data']
with tempfile.TemporaryDirectory(prefix='kubik-render-emoji-') as temporary:
    for target in ('ESP32C6', 'ESP32S3'):
        binary = Path(temporary) / target.lower()
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', f'-DCONFIG_IDF_TARGET_{target}=1',
            '-Ifirmware/main', 'firmware/sim/render_emoji_test.c',
            *[f'firmware/main/{name}.c' for name in SOURCES], '-lm', '-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
