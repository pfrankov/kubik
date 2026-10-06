#!/usr/bin/env python3
"""Render an 18-second Tess conversation from the native renderer (synthetic playback envelope)."""
from pathlib import Path
import argparse
import os
import runpy
import subprocess
import tempfile
from frame_diff import write_png, side_by_side

ROOT = Path(__file__).resolve().parent.parent
SIZE = 480
BYTES = SIZE * SIZE * 3

def build(exe, before=None):
    sources = runpy.run_path(str(ROOT / 'tools/test-frames.py'))['SOURCES']
    paths = []
    for source in sources:
        path = ROOT / 'firmware' / (source + '.c')
        if before and source in ['main/tess_motion', 'main/tess_geometry', 'main/tess_draw']:
            path = before / path.name
        paths.append(str(path))
    subprocess.run(['cc', '-O2', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                    '-Ifirmware/managed_components/espressif__qrcode', '-Ifirmware/main', *paths, '-lm', '-o', str(exe)],
                   cwd=ROOT, check=True)

def capture(exe, out, live=False):
    environment = {**os.environ, 'SIM_CHARACTER': 'tess',
                   'KUBIK_SPRITES': str(ROOT / 'firmware/assets/sprites.bin')}
    sim = subprocess.Popen([str(exe), 'dialogue', *(['live'] if live else [])], stdout=subprocess.PIPE, env=environment)
    movie = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                '-s', '480x480', '-r', '30', '-i', '-', '-vf',
                'fps=15,scale=320:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse',
                str(out / 'conversation.gif')], stdin=subprocess.PIPE)
    snapshots = {60: 'idle', 150: 'waiting', 255: 'reply', 295: 'reply-pause', 520: 'return'}
    if live:
        snapshots = {15: 'connecting', 80: 'mic', 150: 'waiting', 225: 'reply', 310: 'mic-resumed', 350: 'return'}
    frames = {}
    try:
        for index in range(360 if live else 540):
            raw = sim.stdout.read(BYTES)
            if len(raw) != BYTES:
                raise ValueError(f'Incomplete native frame {index}')
            movie.stdin.write(raw)
            if index in snapshots:
                name = snapshots[index]
                write_png(out / (name + '.png'), SIZE, SIZE, raw)
                frames[name] = raw
        if sim.stdout.read(1):
            raise ValueError('Extra native frame')
        assert sim.wait() == 0
        movie.stdin.close()
        assert movie.wait() == 0
    finally:
        for process in [sim, movie]:
            if process.poll() is None:
                process.kill(); process.wait()
    write_png(out / 'conversation.png', SIZE * 3, SIZE,
              side_by_side([frames[name] for name in (['mic', 'waiting', 'reply'] if live else ['idle', 'waiting', 'reply'])]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('/tmp/kubik-dialogue'))
    parser.add_argument('--before', type=Path, help='Three saved pre-change Tess source files')
    parser.add_argument('--live', action='store_true', help='Persistent GPT Live microphone/playback flow')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='kubik-dialogue-') as temp:
        exe = Path(temp) / 'sim'
        build(exe, args.before); capture(exe, args.out, args.live)
    print(args.out)

if __name__ == '__main__':
    main()
