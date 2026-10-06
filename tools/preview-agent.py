#!/usr/bin/env python3
"""Render native Agent UI fixtures as 480x480 PNGs for design review."""
from pathlib import Path
import argparse
import os
import runpy
import subprocess
import tempfile
from frame_diff import write_png, side_by_side

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, default=Path('/tmp/kubik-agent-design'))
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
sources = runpy.run_path(str(ROOT / 'tools/test-render.py'))['TESTS']['render_regression']
sources = [*sources, 'agent_menu', 'face_agent', 'ui_text']
with tempfile.TemporaryDirectory(prefix='kubik-agent-preview-') as tmp:
    exe = Path(tmp) / 'preview'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Ifirmware/main',
                    '-Ifirmware/sim/present_stubs', 'firmware/sim/agent_preview.c',
                    *[f'firmware/main/{name}.c' for name in sources], '-lm', '-o', str(exe)],
                   cwd=ROOT, check=True)
    fixtures = {'events': ['journal'], 'events-empty': ['journal', 'empty'], 'events-detail': ['journal', 'detail'], 'events-overlay': ['journal', 'overlay'], 'events-live': ['journal', 'live'], 'clock': ['clock'], 'clock-low-battery': ['clock', 'low'], 'clock-reminder': ['clock', 'due'], 'overview': ['0'], 'models': ['1'], 'models-long': ['1', '1'],
                'model-saving': ['1', '0', 'pending'], 'model-error': ['1', '0', 'error'],
                'input': ['1', '0', 'input'], 'output': ['1', '0', 'output'], 'input-missing': ['1', '0', 'input-missing'],
                'offline': ['0', '0', 'offline'], 'settings': ['settings'], 'settings-muted': ['settings', '0'], 'sound': ['settings', '70', 'sound'],
                'guide-welcome': ['2', '0'], 'guide-input': ['2', '1'], 'guide-output': ['2', '2'],
                'guide-settings': ['2', '3'], 'guide-missing-stt': ['2', '1', 'missing'],
                'guide-missing-tts': ['2', '2', 'missing'], 'guide-quiet': ['2', '2', 'quiet'],
                'guide-entering': ['2', '1', 'entering'],
                'voice-modes': ['3', '0', 'realtime-modes'],
                'voice-modes-missing': ['3', '0', 'missing-modes'],
                'voice-classic': ['4', '0'],
                'voice-models': ['1', '0', 'realtime-picker'],
                'input-live': ['1', '0', 'live-picker'], 'input-realtime': ['1', '0', 'realtime-picker'],
                'guide-live': ['2', '1', 'live'], 'guide-realtime': ['2', '1', 'realtime'],
                'guide-plush': ['2', '1', 'plush'], 'guide-plush-live': ['2', '1', 'plush-live'],
                'guide-live-missing': ['2', '1', 'live-missing'],
                'guide-offline': ['2', '1', 'offline']}
    for state in ['connecting', 'mic', 'mic-quiet', 'mic-low', 'thinking', 'speaking', 'mic-card', 'speaking-card', 'mic-charging']:
        fixtures['live-' + state] = ['live', state]
    frames = {}
    for name, flags in fixtures.items():
        raw = subprocess.check_output([str(exe), *flags], cwd=ROOT)
        if len(raw) != 480 * 480 * 3:
            raise ValueError(f'Incomplete native frame: {name}')
        write_png(args.out / f'{name}.png', 480, 480, raw)
        frames[name] = raw
    write_png(args.out / 'overview-models.png', 960, 480,
              side_by_side([frames['overview'], frames['models']]))
    write_png(args.out / 'settings-agent.png', 960, 480,
              side_by_side([frames['settings'], frames['overview']]))
    write_png(args.out / 'guide.png', 1920, 480,
              side_by_side([frames[name] for name in ['guide-welcome', 'guide-input', 'guide-output', 'guide-settings']]))
    print(args.out)
