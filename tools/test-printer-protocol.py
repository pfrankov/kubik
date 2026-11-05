#!/usr/bin/env python3
"""Check the production printer RPC boundary with the real application RX/tick hooks."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf-v5.5.1"))
CJSON = IDF / "components/json/cJSON"


def function(source, start, following):
    text = (ROOT / source).read_text()
    return text[text.index(start):text.index(following, text.index(start))]


with tempfile.TemporaryDirectory(prefix="kubik-printer-") as temporary:
    directory = Path(temporary)
    fixture = (ROOT / "firmware/sim/printer_protocol_test.c").read_text()
    fixture = fixture.replace("/* PRODUCTION_ON_JSON */", function(
        "firmware/main/app_protocol.c", "static void on_json(", "static void on_audio("))
    fixture = fixture.replace("/* PRODUCTION_APP_TICK */", function(
        "firmware/main/app_runtime.c", "void app_tick(void)", "static void setup_result_face("))
    source = directory / "printer.c"
    source.write_text(fixture)
    for target in ("ESP32C6", "ESP32S3"):
        binary = directory / target.lower()
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f"-DCONFIG_IDF_TARGET_{target}=1", "-Ifirmware/main", "-Ifirmware/sim/settings_stubs",
            "-Ifirmware/sim/display_s3_stubs", f"-I{CJSON}", str(source),
            "firmware/main/printer_protocol.c", "firmware/main/link_hello.c", str(CJSON / "cJSON.c"),
            "-o", str(binary),
        ], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)
print("Printer RPC: both targets passed ASan/UBSan using production parser, hello and app tick")
