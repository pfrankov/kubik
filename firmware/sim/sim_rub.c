// Simulator scenarios for petting with effort (rub.c): a finger gesture, then how the character answers.
//   sim rub            every script, one after another (the frame-hash stream)
//   sim rub <name>     one script (swipe, slow, vigorous, stop, corner)
// The stage reached is logged on stderr; SIM_CHARACTER picks Plush or Tess.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rub_script.h"
#include "sim.h"

static void run_script(int script, int character) {
    face_t f;
    face_init(&f);
    face_set_character(&f, character);
    face_set_mode(&f, MODE_IDLE);
    f.body.req = -1;
    for (int i = 0; i < 3 * R_FPS; i++) face_update(&f, 1.f / R_FPS);
    rub_finger_t finger = {0};
    int frames = (int)(k_script_s[script] * R_FPS), stage = -1;
    for (int i = 0; i < frames; i++) {
        float t = (float)i / R_FPS;
        rub_script_feed(&f, &finger, script, t);
        face_update(&f, 1.f / R_FPS);
        if (f.rub.stage != stage) {
            stage = f.rub.stage;
            fprintf(stderr, "  %s %5.2f s: stage %d (energy %.2f)\n", k_script_names[script], t, stage, f.rub.energy);
        }
        sim_render(&f);
        sim_emit_frame();
    }
    fprintf(stderr, "rub %-9s %d frames, peak stage %d, joys %d\n", k_script_names[script], frames, f.rub.peak, f.rub.joys);
}

void sim_rub(const char *name, int character) {
    bool found = !name;
    for (int i = 0; i < SCRIPT_COUNT; i++) {
        if (name && strcmp(name, k_script_names[i])) continue;
        if (!name && i == SCRIPT_CORNER) continue;  // a test, not part of the reference stream
        run_script(i, character);
        found = true;
    }
    if (!found) {
        fprintf(stderr, "no script %s\n", name);
        exit(1);
    }
}
