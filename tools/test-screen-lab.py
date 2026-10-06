#!/usr/bin/env python3
"""Portable Screen Lab acceptance: actual native controller and renderer, both characters."""
from pathlib import Path
import runpy
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
sources = runpy.run_path(str(ROOT / "tools/test-frames.py"))["SOURCES"]
sources = [item for item in sources if not item.startswith("sim/")]
sources += ["main/screen_lab", "main/screen_lab_events", "main/screen_lab_fixture", "main/screen_lab_draw", "sim/screen_lab_test"]
with tempfile.TemporaryDirectory(prefix="kubik-screen-lab-") as directory:
    output = Path(directory) / "test"
    subprocess.run(["cc", "-O1", "-g", "-fsanitize=address,undefined", "-Ifirmware/main",
                    "-Ifirmware/managed_components/espressif__qrcode",
                    *[f"firmware/{item}.c" for item in sources], "-lm", "-o", str(output)], cwd=ROOT, check=True)
    subprocess.run([str(output)], cwd=ROOT, check=True)
