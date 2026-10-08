#!/usr/bin/env python3
"""Native game contracts and real app tap routing; no microphone, network or device."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="kubik-games-") as directory:
    temp = Path(directory)
    control = (ROOT / "firmware/main/app_control.c").read_text()
    tap = control[control.index("static void touch_tap("):control.index("static void touch_pet(")]
    app = (ROOT / "firmware/main/app.c").read_text()
    game_taps = app[app.index("typedef struct { int32_t x, y; uint32_t round; } game_tap_t;"):app.index("static void app_task(")]
    fixture = (ROOT / "firmware/sim/tess_game_touch_test.c").read_text()
    app_test = temp / "touch.c"
    app_test.write_text(fixture.replace("/* PRODUCTION_GAME_TAPS */", game_taps).replace("/* PRODUCTION_TAP */", tap))
    for name, source, defines in (("model", ROOT / "firmware/sim/tess_games_test.c", []),
                                   ("touch", app_test, []), ("plush-touch", app_test, ["-DKUBIK_CHARACTER=0"])):
        exe = temp / name
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                        "-fsanitize=address,undefined", "-Ifirmware/main", *defines, str(source),
                        "firmware/main/tess_games.c", "firmware/main/app_state.c", "-lm", "-o", str(exe)],
                       cwd=ROOT, check=True)
        subprocess.run([str(exe)], cwd=ROOT, check=True)
