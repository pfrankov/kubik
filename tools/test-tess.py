#!/usr/bin/env python3
"""Test Tess's procedural sound family (composer, synth, rate limits) with ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="kubik-tess-") as tmp:
    exe = str(Path(tmp) / "test")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "firmware/sim/tess_sound_test.c",
                    "firmware/sim/tess_sound_rules_test.c", "firmware/sim/tess_sound_cost_test.c",
                    "firmware/sim/tess_sound_profiles_test.c", "firmware/main/tess_sound.c",
                    "firmware/main/tess_sound_compose.c", "firmware/main/tess_sound_rules.c", "-lm", "-o", exe],
                   cwd=ROOT, check=True)
    subprocess.run([exe], cwd=ROOT, check=True)
