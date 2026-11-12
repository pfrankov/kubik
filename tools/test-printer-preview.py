#!/usr/bin/env python3
"""Exercise bounded preview transfer and snapshot lifetime on both profiles."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='kubik-preview-') as directory:
    (Path(directory) / 'sdkconfig.h').write_text('')
    for target in ('ESP32C6', 'ESP32S3'):
        binary = Path(directory) / target
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-DESP_PLATFORM', f'-DCONFIG_IDF_TARGET_{target}=1',
                        f'-I{directory}',
                        '-Ifirmware/main', '-Ifirmware/sim/display_s3_stubs',
                        'firmware/main/printer_preview.c', 'firmware/main/render_printer_preview.c',
                        'firmware/sim/printer_preview_test.c',
                        '-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
