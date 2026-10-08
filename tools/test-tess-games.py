#!/usr/bin/env python3
"""Native game contracts and real app tap routing; no microphone, network or device."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="kubik-games-") as directory:
    temp = Path(directory)
    # Production touch sampling wrapper: a crashed USB client cannot hold a finger down.
    (temp / "freertos").mkdir()
    (temp / "freertos/FreeRTOS.h").write_text("#include <pthread.h>\ntypedef pthread_mutex_t portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n#define portENTER_CRITICAL(p) ((void)pthread_mutex_lock(p))\n#define portEXIT_CRITICAL(p) ((void)pthread_mutex_unlock(p))\n")
    (temp / "board.h").write_text("#include <stdbool.h>\nbool touch_read(int *, int *);\n")
    (temp / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
    # Quoted includes resolve next to the source first; copy this wrapper alone, not hardware headers.
    (temp / "input_probe.c").write_text((ROOT / "firmware/main/input_probe.c").read_text())
    probe = temp / "probe"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-pthread", "-I" + str(temp), "-Ifirmware/main",
                    "firmware/sim/input_probe_test.c", str(temp / "input_probe.c"), "-o", str(probe)], cwd=ROOT, check=True)
    subprocess.run([str(probe)], check=True)
    assert "bool has_touch = input_probe_read(&x, &y);" in (ROOT / "firmware/main/input.c").read_text()
    control = (ROOT / "firmware/main/app_control.c").read_text()
    tap = control[control.index("static void touch_tap("):control.index("static void touch_pet(")]
    app = (ROOT / "firmware/main/app.c").read_text()
    game_taps = app[app.index("typedef struct { int32_t x, y; uint32_t round; } game_tap_t;"):app.index("static void app_task(")]
    fixture = (ROOT / "firmware/sim/tess_game_touch_test.c").read_text()
    app_test = temp / "touch.c"
    app_test.write_text(fixture.replace("/* PRODUCTION_GAME_TAPS */", game_taps).replace("/* PRODUCTION_TAP */", tap))
    # Isolated state-machine/app-route tests do not run the renderer or speaker.
    # Production feedback is exercised by tess_games_integration_test.c.
    support = temp / "feedback.c"
    support.write_text('#include "tess_internal.h"\n'
                       'void tess_touch_wave(face_t *f,float x,float y){(void)f;(void)x;(void)y;}\n'
                       'void tess_touch_resume(face_t *f,float x,float y,float age){(void)f;(void)x;(void)y;(void)age;}\n'
                       'void tess_cue(face_t *f,tess_cue_t c,float s,float p){(void)f;(void)c;(void)s;(void)p;}\n')
    for name, source, defines in (("model", ROOT / "firmware/sim/tess_games_test.c", []),
                                   ("touch", app_test, []), ("plush-touch", app_test, ["-DKUBIK_CHARACTER=0"])):
        exe = temp / name
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                        "-fsanitize=address,undefined", "-Ifirmware/main", *defines, str(source),
                        "firmware/main/tess_games.c", str(support), "firmware/main/app_state.c", "-lm", "-o", str(exe)],
                       cwd=ROOT, check=True)
        subprocess.run([str(exe)], cwd=ROOT, check=True)
