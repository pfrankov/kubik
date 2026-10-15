#!/usr/bin/env python3
"""Apply only the audited pinned SDK change; reject drift instead of guessing."""
from pathlib import Path
import hashlib
import subprocess
import sys

SOURCE = "18041ec37d3456bc1ecb3275a78c3ae64679f57f049b5b05af169ef6b1857c77"
PATCHED = "e732456147b00bbdc8c9e8387af0e74921f8d7f96500859ee824fe45f64d9eb7"

def apply(directory):
    source = Path(directory) / "esp_websocket_client.c"
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest == PATCHED:
        return
    if digest != SOURCE:
        raise RuntimeError("WebSocket SDK changed: review the duplex patch before building")
    patch = Path(__file__).parent / "fixtures/websocket-duplex.patch"
    subprocess.run(["patch", "--batch", "-p0", "-i", str(patch.resolve())], cwd=directory, check=True)
    if hashlib.sha256(source.read_bytes()).hexdigest() != PATCHED:
        raise RuntimeError("WebSocket patch verification failed")

if __name__ == "__main__":
    apply(sys.argv[1])
