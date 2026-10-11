#!/usr/bin/env python3
"""Portable Screen Lab acceptance: actual native controller and renderer, both characters."""
from pathlib import Path
import re
import runpy
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent


def quoted_values(path, pattern):
    match = re.search(pattern, path.read_text(), re.DOTALL)
    assert match, f"Missing Screen Lab catalog in {path.relative_to(ROOT)}"
    return re.findall(r'[\"\']([^\"\']+)[\"\']', match.group(1))


firmware_names = quoted_values(
    ROOT / "firmware/main/screen_lab_fixture.c",
    r"static const char \*const names\[LAB_COUNT\] = \{(.*?)\};",
)
device_names = quoted_values(
    ROOT / "tools/test-device-screen-lab.mjs",
    r"const names=\[(.*?)\];",
)
assert device_names == firmware_names, "Device Screen Lab catalog differs from firmware"

sources = runpy.run_path(str(ROOT / "tools/test-frames.py"))["SOURCES"]
sources = [item for item in sources if not item.startswith("sim/")]
sources += ["main/screen_lab", "main/screen_lab_events", "main/screen_lab_fixture", "main/screen_lab_draw", "sim/screen_lab_test"]
with tempfile.TemporaryDirectory(prefix="kubik-screen-lab-") as directory:
    output = Path(directory) / "test"
    subprocess.run(["cc", "-O1", "-g", "-fsanitize=address,undefined", "-Ifirmware/main",
                    "-Ifirmware/managed_components/espressif__qrcode",
                    *[f"firmware/{item}.c" for item in sources], "-lm", "-o", str(output)], cwd=ROOT, check=True)
    subprocess.run([str(output)], cwd=ROOT, check=True)
