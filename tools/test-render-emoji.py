#!/usr/bin/env python3
"""Check mixed native color emoji rendering, scene ownership and dirty signatures."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['render_scene', 'render_text', 'render_overlay_s3', 'render_projection', 'render_emoji', 'render_emoji_store', 'emoji_cluster', 'font', 'font_data']
with tempfile.TemporaryDirectory(prefix='kubik-render-emoji-') as temporary:
    stubs=Path(temporary)/'stubs'
    stubs.mkdir()
    (stubs/'sdkconfig.h').write_text('')
    (stubs/'esp_heap_caps.h').write_text('#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_INTERNAL 1u\n#define MALLOC_CAP_8BIT 2u\n#define MALLOC_CAP_DMA 4u\n#define MALLOC_CAP_SPIRAM 8u\nvoid *heap_caps_malloc(size_t,unsigned);\nvoid heap_caps_free(void*);\n')
    for target in ('ESP32C6', 'ESP32S3'):
        binary = Path(temporary) / target.lower()
        store_object=Path(temporary)/(target.lower()+'-store.o')
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-Dmalloc=emoji_test_malloc','-Dfree=emoji_test_free',
            '-Ifirmware/main','-c','firmware/main/render_emoji_store.c','-o',str(store_object)],cwd=ROOT,check=True)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', f'-DCONFIG_IDF_TARGET_{target}=1',
            '-Ifirmware/main', 'firmware/sim/render_emoji_test.c',
            *[f'firmware/main/{name}.c' for name in SOURCES if name!='render_emoji_store'],str(store_object),
            '-lm', '-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
        store_binary=Path(temporary)/(target.lower()+'-store')
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-DESP_PLATFORM=1',f'-DCONFIG_IDF_TARGET_ESP32S3={int(target=="ESP32S3")}',
            '-Ifirmware/main',f'-I{stubs}','firmware/sim/render_emoji_store_test.c','firmware/main/render_emoji_store.c',
            '-o',str(store_binary)],cwd=ROOT,check=True)
        subprocess.run([str(store_binary)],cwd=ROOT,check=True,timeout=10)
