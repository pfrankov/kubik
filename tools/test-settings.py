#!/usr/bin/env python3
"""Exercise Wi-Fi and server settings.c pairs against fault-injecting NVS stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="kubik-settings-") as tmp:
    exe = str(Path(tmp) / "settings-test")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-unused-variable", "-fsanitize=address,undefined",
        "-Ifirmware/sim/settings_stubs", "firmware/sim/settings_test.c",
        "firmware/main/settings.c", "-o", exe,
    ], cwd=ROOT, check=True)
    subprocess.run([exe], cwd=ROOT, check=True)
