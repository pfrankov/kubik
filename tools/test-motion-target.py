#!/usr/bin/env python3
"""Exercise actual IMU byte decoding/fusion with each board's screen-frame rotation."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "firmware/main/input.c").read_text()
fixture = (ROOT / "tools/fixtures/input_motion_target_test.c").read_text()

def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]

fixture = fixture.replace("/* PRODUCTION_IMU_READ */", section("static bool imu_read(", "// ------------------------------------------------------------------ state"))
fixture = fixture.replace("/* PRODUCTION_PULL_SIGNS */", section("#define IMU_PULL_X_SIGN", "static volatile bool s_ptt"))
fixture = fixture.replace("/* PRODUCTION_FUSION */", section("static void initialize_gravity(", "static void update_viewpoint("))
with tempfile.TemporaryDirectory(prefix="kubik-motion-target-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(fixture)
    for target in ("esp32c6", "esp32s3"):
        executable = path / target
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Ifirmware/main",
                        f"-DCONFIG_IDF_TARGET_ESP32S3={int(target == 'esp32s3')}",
                        str(path / "test.c"), "firmware/main/viewpoint.c", "-lm", "-o", str(executable)],
                       cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
