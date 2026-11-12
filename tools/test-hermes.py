#!/usr/bin/env python3
"""Hermes plugin checks: no agent generation or paid speech providers."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
subprocess.run(['uv', 'run', '--python', '3.14', '--with', 'aiohttp==3.14.3',
                '--with', 'cryptography==50.0.0', '--with', 'pyyaml==6.0.3', '--with', 'ruamel-yaml==0.18.16',
                '--with', 'wasmtime==50.0.1', '--with', 'regex==2026.9.10', '--with', 'pillow==12.3.0', 'python',
                '-m', 'unittest', 'discover', '-s', str(ROOT / 'tools/fixtures'),
                '-p', 'hermes_*_test.py', '-v'], cwd=ROOT, check=True)
