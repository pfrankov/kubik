#!/usr/bin/env python3
"""Render offline rolling contacts using native physics and firmware synthesis."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='Output mono WAV, at Interface 100%')
    args = parser.parse_args()
    sources = runpy.run_path(str(ROOT / 'tools/test-render.py'))['face_sources']()
    sources += ['agent_menu', 'face_agent', 'ui_text', 'tess_sound', 'tess_sound_compose', 'tess_sound_rules']
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='kubik-particles-') as temporary:
        exe = str(Path(temporary) / 'render')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', 'firmware/sim/tess_particles_render.c',
                        *[f'firmware/main/{name}.c' for name in sources], '-lm', '-o', exe], cwd=ROOT, check=True)
        subprocess.run([exe, str(output)], cwd=ROOT, check=True)
    print(output)

if __name__ == '__main__':
    main()
