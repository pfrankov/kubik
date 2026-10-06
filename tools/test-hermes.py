#!/usr/bin/env python3
"""Hermes plugin checks: no agent generation or paid speech providers."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
subprocess.run(['uv', 'run', '--python', '3.11', '--with', 'aiohttp==3.14.3',
                '--with', 'cryptography==50.0.0', '--with', 'pyyaml==6.0.3', 'python',
                str(ROOT / 'tools/fixtures/hermes_plugin_test.py')], cwd=ROOT, check=True)
