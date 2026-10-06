#!/usr/bin/env python3
"""Run native Agent menu state, rendering, and cJSON protocol checks."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
IDF = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf-v5.5.1"))
CJSON = IDF / "components/json/cJSON"


def build_test(build, name, test, sources, extra=()):
    exe = Path(build) / name
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Ifirmware/main", f"-I{CJSON}", *extra,
        test, *sources, "-o", str(exe),
    ], cwd=ROOT, check=True)
    subprocess.run([str(exe)], cwd=ROOT, check=True)


with tempfile.TemporaryDirectory(prefix="kubik-agent-") as tmp:
    build_test(tmp, "state-ui", "firmware/sim/agent_menu_test.c", [
        "firmware/main/agent_menu.c", "firmware/main/face_agent.c",
        "firmware/main/render.c", "firmware/main/render_dots.c", "firmware/main/render_glass.c",
        "firmware/main/render_output.c", "firmware/main/render_pipeline.c", "firmware/main/render_raster.c",
        "firmware/main/render_scene.c", "firmware/main/render_text.c", "firmware/main/font.c",
        "firmware/main/font_data.c", "-lm",
    ], extra=("-Ifirmware/sim/present_stubs",))
    build_test(tmp, "protocol", "firmware/sim/agent_protocol_test.c", [
        "firmware/main/agent_protocol.c", "firmware/main/agent_menu.c", str(CJSON / "cJSON.c"),
    ])

    build_test(tmp, "hidden-reset", "firmware/sim/menu_service_test.c", [])
