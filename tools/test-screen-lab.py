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
assert len(firmware_names) == 33, "Firmware Screen Lab catalog must cover all 33 screens"
assert device_names == firmware_names, "Device Screen Lab catalog differs from firmware"

sources = runpy.run_path(str(ROOT / "tools/test-frames.py"))["SOURCES"]
sources = [item for item in sources if not item.startswith("sim/")]
sources += ["main/screen_lab", "main/screen_lab_events", "main/screen_lab_fixture", "main/screen_lab_draw"]
with tempfile.TemporaryDirectory(prefix="kubik-screen-lab-") as directory:
    for fixture, target in (("screen_lab_test", 0), ("screen_lab_rectangle_test", 1)):
        output = Path(directory) / fixture
        subprocess.run(["cc", "-O1", "-g", "-fsanitize=address,undefined", "-Ifirmware/main",
                        "-Ifirmware/managed_components/espressif__qrcode", f"-DCONFIG_IDF_TARGET_ESP32S3={target}",
                        *[f"firmware/{item}.c" for item in sources], f"firmware/sim/{fixture}.c",
                        "-lm", "-o", str(output)], cwd=ROOT, check=True)
        subprocess.run([str(output)], cwd=ROOT, check=True)
