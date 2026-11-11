#!/usr/bin/env python3
"""Check private emoji atlas format/generation without bundling Apple artwork."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
for test in ('test-atlas.py', 'test-color-atlas.py'):
    subprocess.run([sys.executable, str(ROOT / 'tools/emoji' / test)],
                   cwd=ROOT, check=True, timeout=60)
