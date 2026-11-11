#!/usr/bin/env python3
"""Check private emoji atlas format/generation without bundling Apple artwork."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(ROOT / 'tools/emoji/test-atlas.py')],
               cwd=ROOT, check=True, timeout=60)
