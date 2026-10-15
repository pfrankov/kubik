#!/usr/bin/env python3
"""Run the production frame snapshot mailbox with deterministic task/USB delays."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="kubik-diagnostics-") as directory:
    source = (ROOT / "firmware/main/frame_diagnostics.c").read_text()
    source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
    main = (ROOT / "firmware/main/main.c").read_text()
    cadence = main[main.index("static int frame_rate("):main.index("// Sleeps to the next frame")]
    fixture = (ROOT / "firmware/sim/frame_diagnostics_test.c").read_text().replace("/* PRODUCTION */", source).replace("/* CADENCE */", cadence)
    path = Path(directory) / "test.c"
    path.write_text(fixture)
    exe = Path(directory) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-Ifirmware/main", str(path), "firmware/main/status_json.c",
                    "-o", str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=ROOT, check=True)
