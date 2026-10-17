#!/usr/bin/env python3
"""Check all Tess sounds with sanitizers; measure the same cost inputs without instrumentation."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
BASE = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra"]
SANITIZERS = ["-fsanitize=address,undefined"]
SYNTH = ["firmware/main/tess_sound.c", "firmware/main/tess_sound_compose.c",
         "firmware/main/tess_sound_body.c", "firmware/main/tess_sound_sky.c", "firmware/main/tess_sound_rules.c"]
COST = ["firmware/sim/tess_sound_cost_main.c", "firmware/sim/tess_sound_cost_test.c"]
SEMANTICS = ["firmware/sim/tess_sound_test.c", "firmware/sim/tess_sound_rules_test.c",
             "firmware/sim/tess_sound_profiles_test.c", "firmware/sim/tess_sound_body_test.c"]
with tempfile.TemporaryDirectory(prefix="kubik-tess-") as tmp:
    for name, flags, sources in (("semantics", SANITIZERS, SEMANTICS),
                                  ("cost-safety", SANITIZERS + ["-DKUBIK_COST_INSTRUMENTED"], COST),
                                  ("cost", [], COST)):
        exe = str(Path(tmp) / name)
        subprocess.run(BASE + flags + sources + SYNTH + ["-lm", "-o", exe], cwd=ROOT, check=True)
        print(f"Tess probe: {name}", flush=True)
        subprocess.run([exe], cwd=ROOT, check=True)
