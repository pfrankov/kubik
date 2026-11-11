#!/usr/bin/env python3
"""Shared screen/printer Unicode cluster boundaries; no artwork, platform APIs or I/O at runtime."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/emoji'))
from atlas import pinned_sequences

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--unicode-source', type=Path, help='Also test every sequence in the pinned Unicode 17 data')
args = parser.parse_args()
spec = importlib.util.spec_from_file_location('generate_cluster', ROOT / 'tools/emoji/generate-cluster.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
assert generator.ranges([3, 8, 2, 4, 8]) == [[2, 4], [8, 8]]
try:
    generator.generate(b'changed Unicode source')
    raise AssertionError('Unpinned Unicode source accepted')
except ValueError:
    pass
with tempfile.TemporaryDirectory(prefix='kubik-emoji-cluster-') as temporary:
    directory = Path(temporary)
    extra = []
    if args.unicode_source:
        data = args.unicode_source.read_bytes()
        assert generator.generate(data) == (ROOT / 'firmware/main/emoji_cluster_data.inc').read_text()
        rows = ['    "' + ''.join(f'\\x{byte:02x}' for byte in value.encode()) + '",' for value in pinned_sequences(data)]
        (directory / 'emoji_cluster_vectors.h').write_text('static const char *const unicode_sequences[] = {\n' + '\n'.join(rows) + '\n};\n')
        extra.append('-DEMOJI_OFFICIAL_VECTORS')
    binary = directory / 'cluster'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Ifirmware/main',
                    '-I' + str(directory), *extra, 'firmware/sim/emoji_cluster_test.c',
                    'firmware/main/emoji_cluster.c', '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
print('Shared emoji scanner passed ASan/UBSan; generation is pinned and ordinary digits remain text')
