#!/usr/bin/env python3
"""Exercise real presentation receipts and screen image cancellation."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
idf = Path(os.environ.get('IDF_PATH', str(Path.home() / 'esp/esp-idf-v5.5.1')))
cjson = idf / 'components/json/cJSON'
with tempfile.TemporaryDirectory(prefix='screen-image-') as tmp:
    for target in ('ESP32C6', 'ESP32S3'):
        (Path(tmp) / 'sdkconfig.h').write_text(f'#define CONFIG_IDF_TARGET_{target} 1\n')
        binary = Path(tmp) / target
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', '-pthread', '-fsanitize=address,undefined', '-DESP_PLATFORM', f'-I{tmp}',
                        '-Ifirmware/main', '-Ifirmware/sim/settings_stubs', '-Ifirmware/sim/display_s3_stubs',
                        '-I' + str(cjson), 'firmware/main/printer_preview.c', 'firmware/main/screen_image.c',
                        'firmware/sim/screen_image_test.c', str(cjson / 'cJSON.c'), '-o', str(binary)],
                       cwd=root, check=True)
        subprocess.run([str(binary)], cwd=root, check=True, timeout=10)
        source = (root / 'firmware/main/main.c').read_text()
        function = source[source.index('static void note_preview_presented('):source.index('static void display_task(')]
        fixture = (root / 'firmware/sim/screen_image_present_test.c').read_text().replace(
            '/* PRODUCTION_PRESENT_RECEIPT */', function)
        harness = Path(tmp) / (target + '.c')
        harness.write_text(fixture)
        receipt = Path(tmp) / (target + '-receipt')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsanitize=address,undefined', f'-DCONFIG_IDF_TARGET_ESP32S3={int(target == "ESP32S3")}',
                        str(harness), '-o', str(receipt)], cwd=root, check=True)
        subprocess.run([str(receipt)], cwd=root, check=True, timeout=10)
        draw = source[source.index('static void draw_preview('):source.index('static face_mode_t update_scene(')]
        geometry = (root / 'firmware/sim/screen_image_geometry_test.c').read_text().replace(
            '/* PRODUCTION_DRAW_PREVIEW */', draw)
        geometry_source = Path(tmp) / (target + '-geometry.c')
        geometry_source.write_text(geometry)
        geometry_bin = Path(tmp) / (target + '-geometry')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DUI_H={584 if target == "ESP32S3" else 480}',
                        str(geometry_source), '-o', str(geometry_bin)], cwd=root, check=True)
        subprocess.run([str(geometry_bin)], cwd=root, check=True, timeout=10)
