#!/usr/bin/env python3
"""Check all Tess sounds with sanitizers; measure the same cost inputs without instrumentation."""
from pathlib import Path
import os
import signal
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


def check_cost_faults(directory):
    exe = str(Path(directory) / 'cost-faults')
    source = 'firmware/sim/tess_sound_cost_fault_test.c'
    subprocess.run(BASE + SANITIZERS + [source] + SYNTH + ['-lm', '-o', exe], cwd=ROOT, check=True)
    # Cases run the real probe with deterministic timings; failures must be its
    # intended assertion, not a sanitizer error, missing executable or crash.
    failures = {3: 'slow.total <= COST_SLOW_KEPT', 4: 'confirmed == 0',
                5: 'confirmed == 0', 8: 'confirmed == 0', 9: 'confirmed == 0',
                10: 'matches', 12: 'matches'}
    for case in range(13):
        result = subprocess.run([exe, str(case)], cwd=ROOT, text=True, capture_output=True)
        expected = failures.get(case)
        if expected:
            assert result.returncode == -signal.SIGABRT and expected in result.stderr, result.stderr
        else:
            assert result.returncode == 0, result.stderr
        assert 'runtime error:' not in result.stderr and 'ERROR: AddressSanitizer' not in result.stderr, result.stderr
        if case == 3:
            assert 'synth: replay CPU' not in result.stderr, 'Widespread overruns must fail without retrying'
    print('Tess cost contract: 13 transient, persistent, boundary and PCM cases passed', flush=True)


with tempfile.TemporaryDirectory(prefix="kubik-tess-") as tmp:
    check_cost_faults(tmp)
    for name, flags, sources in (("semantics", SANITIZERS, SEMANTICS),
                                  ("cost-safety", SANITIZERS + ["-DKUBIK_COST_INSTRUMENTED"], COST),
                                  ("cost", [], COST)):
        exe = str(Path(tmp) / name)
        subprocess.run(BASE + flags + sources + SYNTH + ["-lm", "-o", exe], cwd=ROOT, check=True)
        print(f"Tess probe: {name}", flush=True)
        subprocess.run([exe], cwd=ROOT, check=True)
