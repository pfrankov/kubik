#!/usr/bin/env python3
"""Native touch path and the exact hash-checked SDK change; no attached hardware."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def compile_test(sources, output, includes):
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-pthread", *["-I" + str(p) for p in includes],
                    *map(str, sources), "-lm", "-o", str(output)], cwd=ROOT, check=True)
    subprocess.run([str(output)], check=True)


def sdk_change(directory, temp):
    patcher = runpy.run_path(str(ROOT / "tools/patch-cst9217.py"))
    source = temp / "esp_lcd_touch_cst9217.c"
    source.write_bytes((directory / source.name).read_bytes())
    if hashlib.sha256(source.read_bytes()).hexdigest() == patcher["PATCHED"]:
        subprocess.run(["patch", "--batch", "-R", "-p0", "-i",
                        str(ROOT / "tools/fixtures/cst9217-multitouch.patch")], cwd=temp, check=True)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == patcher["SOURCE"]
    patcher["apply"](temp)
    original = source.read_bytes()
    patcher["apply"](temp)
    assert source.read_bytes() == original
    source.write_bytes(original + b"\n/* unreviewed SDK */\n")
    try:
        patcher["apply"](temp)
    except RuntimeError:
        assert source.read_bytes() == original + b"\n/* unreviewed SDK */\n"
    else:
        raise AssertionError("Unreviewed driver was patched")
    sdk = original.decode()
    fixture = (ROOT / "tools/fixtures/touch_driver_test.c").read_text()
    functions = {
        "READ_DATA": ("static esp_err_t esp_lcd_touch_cst9217_read_data(esp_lcd_touch_handle_t tp)\n{", "static bool esp_lcd_touch_cst9217_get_xy"),
        "GET_XY": ("static bool esp_lcd_touch_cst9217_get_xy(esp_lcd_touch_handle_t tp,", "static esp_err_t cst9217_get_track_id"),
        "GET_TRACK_ID": ("static esp_err_t cst9217_get_track_id(esp_lcd_touch_handle_t tp,", "static esp_err_t esp_lcd_touch_cst9217_del"),
    }
    for marker, (begin, end) in functions.items():
        start = sdk.rindex(begin)
        finish = sdk.index(end, start + len(begin))
        fixture = fixture.replace("/* " + marker + " */", sdk[start:finish])
    test = temp / "driver.c"
    test.write_text(fixture)
    compile_test([test], temp / "driver", [ROOT / "firmware/main"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-dir", type=Path, default=ROOT / "firmware/managed_components/waveshare__esp_lcd_touch_cst9217")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kubik-touch-") as name:
        temp = Path(name)
        (temp / "freertos").mkdir()
        (temp / "freertos/FreeRTOS.h").write_text("#pragma once\n#include <pthread.h>\ntypedef pthread_mutex_t portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n#define portENTER_CRITICAL(p) ((void)pthread_mutex_lock(p))\n#define portEXIT_CRITICAL(p) ((void)pthread_mutex_unlock(p))\n")
        (temp / "freertos/semphr.h").write_text("typedef void *SemaphoreHandle_t;\n")
        compile_test([ROOT / "firmware/sim/touch_input_test.c", ROOT / "firmware/main/input_touch.c",
                      ROOT / "firmware/main/input_rub.c", ROOT / "firmware/main/rub.c",
                      ROOT / "firmware/main/tess_sky_touch.c"], temp / "touch",
                     [temp, ROOT / "firmware/main"])
        sdk_change(args.sdk_dir, temp)


if __name__ == "__main__":
    main()
