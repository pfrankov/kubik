#!/usr/bin/env python3
"""Expose both verified CST92xx contacts; reject any unreviewed vendor source."""
from pathlib import Path
import hashlib
import subprocess
import sys

# waveshare/esp_lcd_touch_cst9217 1.0.4, commit 43e6e696bbf0cf8671a97fcfe02c4c720e4e4d27.
# Report layout/ACK: https://github.com/lewisxhe/SensorLib/blob/master/src/touch/TouchDrvCST92xx.cpp
# Keep the vendor's initialization, IRQ, retries, reset and coordinate transforms.
SOURCE = "79aa1ffbf7fff8a76a573f3588f51204d3ffe075e5a777b7f7ec5f4e4e357c26"
PATCHED = "0d58f0f8659bb3bef1bd1fde6a460cd8ef615305f1c1ec3dfe035e39599e33b8"


def apply(directory):
    source = Path(directory) / "esp_lcd_touch_cst9217.c"
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest == PATCHED:
        return
    if digest != SOURCE:
        raise RuntimeError("CST9217 SDK changed: review the multitouch patch before building")
    patch = Path(__file__).parent / "fixtures/cst9217-multitouch.patch"
    subprocess.run(["patch", "--batch", "-p0", "-i", str(patch.resolve())], cwd=directory, check=True)
    if hashlib.sha256(source.read_bytes()).hexdigest() != PATCHED:
        raise RuntimeError("CST9217 patch verification failed")


if __name__ == "__main__":
    apply(sys.argv[1])
