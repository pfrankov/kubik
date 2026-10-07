#!/usr/bin/env python3
"""Exercise Wi-Fi and server settings.c pairs against fault-injecting NVS stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="kubik-settings-") as tmp:
    exe = str(Path(tmp) / "settings-test")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-unused-variable", "-fsanitize=address,undefined",
        "-pthread", "-DAPP_NVS_TEST_LOCK", "-DAPP_NVS_TEST_HOOKS",
        "-Dcalloc=settings_test_calloc",
        "-Ifirmware/sim/settings_stubs", "firmware/sim/settings_test.c",
        "firmware/sim/settings_connection_test.c",
        "firmware/sim/settings_persistence_test.c",
        "firmware/main/settings.c", "firmware/main/connection_record.c",
        "firmware/main/connection_store.c",
        "firmware/main/app_nvs.c", "firmware/main/muse_store.c", "-o", exe,
    ], cwd=ROOT, check=True)
    subprocess.run([exe], cwd=ROOT, check=True)
