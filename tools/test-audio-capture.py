#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="c6-audio-capture-") as directory:
    binary = pathlib.Path(directory) / "mic-capture-test"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        str(ROOT / "firmware/sim/mic_capture_test.c"),
        str(ROOT / "firmware/main/mic_capture.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
