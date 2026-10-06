#!/usr/bin/env python3
"""LAN-mode trust on the host: link_pin.c and link_discover.c with real mbedTLS SHA-256."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
IDF = Path(os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf-v5.5.1")))
MBEDTLS = IDF / "components/mbedtls/mbedtls"
CJSON = IDF / "components/json/cJSON"
with tempfile.TemporaryDirectory(prefix="kubik-pin-") as tmp:
    exe = str(Path(tmp) / "pin-test")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
        "-Ifirmware/sim/link_stubs", "-Ifirmware/sim/settings_stubs", "-Ifirmware/main",
        f"-I{MBEDTLS / 'include'}", f"-I{CJSON}",
        "firmware/sim/link_pin_test.c", str(MBEDTLS / "library/sha256.c"),
        str(MBEDTLS / "library/platform_util.c"), str(CJSON / "cJSON.c"), "-o", exe,
    ], cwd=ROOT, check=True)
    subprocess.run([exe], cwd=ROOT, check=True)
