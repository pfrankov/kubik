#!/usr/bin/env python3
"""Render Tess sounds through the native physics and firmware synthesis: offline rolling contacts, or with
--play a scripted play session (plucks, swipes, stretch, throw, gather, roll, knocks, rapid taps) mixed as the
device mixes it, with its peak, loudest 100 ms and biggest rise printed."""
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
    parser.add_argument('--play', action='store_true', help='The play session instead of offline contacts')
    parser.add_argument('--sky', type=int, metavar='ELEMENTS', help='A Skies session from this depth (1..10) instead')
    args = parser.parse_args()
    sources = runpy.run_path(str(ROOT / 'tools/test-render.py'))['face_sources']()
    sources += ['agent_menu', 'face_agent', 'ui_text', 'tess_sound', 'tess_sound_compose', 'tess_sound_body', 'tess_sound_sky', 'tess_sound_rules']
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='kubik-particles-') as temporary:
        exe = str(Path(temporary) / 'render')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', f"firmware/sim/{'tess_sky_render' if args.sky else 'tess_play_render' if args.play else 'tess_particles_render'}.c",
                        *[f'firmware/main/{name}.c' for name in sources], '-lm', '-o', exe], cwd=ROOT, check=True)
        subprocess.run([exe, str(output), *([str(args.sky)] if args.sky else [])], cwd=ROOT, check=True)
    print(output)

if __name__ == '__main__':
    main()
