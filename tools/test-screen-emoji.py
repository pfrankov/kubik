#!/usr/bin/env python3
"""Exercise the actual screen glyph RX, session routing and bounded page requests."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / 'esp/esp-idf-v5.5.1'))
CJSON = IDF / 'components/json/cJSON'
source = (ROOT / 'firmware/main/app_emoji.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include "'))
fixture = (ROOT / 'firmware/sim/screen_emoji_protocol_test.c').read_text().replace('/* PRODUCTION_APP_EMOJI */', source)
modules = ['emoji_protocol', 'emoji_cluster', 'face_card', 'face_emoji', 'render_scene',
           'render_text', 'render_overlay_s3', 'render_projection', 'render_emoji', 'font', 'font_data', 'status_json']
with tempfile.TemporaryDirectory(prefix='kubik-screen-emoji-') as temporary:
    path = Path(temporary) / 'test.c'
    path.write_text(fixture)
    for target in ('ESP32C6', 'ESP32S3'):
        binary = Path(temporary) / target.lower()
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            f'-DCONFIG_IDF_TARGET_{target}=1', '-Ifirmware/main', f'-I{CJSON}', str(path),
            *[f'firmware/main/{name}.c' for name in modules], str(CJSON / 'cJSON.c'), '-lm', '-o', str(binary)],
            cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
