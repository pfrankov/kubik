#include "sim.h"
#include "../main/screen_lab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static screen_lab_t lab;
static scene_t scene;
static int tilt_x, tilt_y = 60;
static void apply_tilt(void) {
    lab.face.grav_x = tilt_x / 100.f; lab.face.grav_y = tilt_y / 100.f;
    float x = lab.face.grav_y * .3f, y = -lab.face.grav_x * .3f;
    lab.face.view_q[0] = cosf(hypotf(x, y));
    lab.face.view_q[1] = sinf(x); lab.face.view_q[2] = sinf(y); lab.face.view_q[3] = 0;
}
static void frame(void) {
        apply_tilt();
        screen_lab_update(&lab, 1.f / R_FPS);
        tess_cue_t cue; float strength, position;
        while (screen_lab_take_cue(&lab, &cue, &strength, &position)) {}
        screen_lab_draw(&lab, &scene);
        sim_render_scene(&scene);
        printf("{\"screen\":%d,\"name\":\"%s\",\"signal\":%s,\"volume\":%d,\"interface\":%d,\"view\":%d,\"journal_open\":%s,\"journal_detail\":%s,\"journal_overlay\":%s,\"journal_offset\":%u,\"journal_count\":%u,\"status_open\":%s,\"status_page\":%u}\n", lab.selected, screen_lab_name(lab.selected), lab.signal ? "true" : "false", lab.volume, lab.ui_volume, lab.face.agent.view, lab.face.journal.open ? "true" : "false", lab.face.journal.detail ? "true" : "false", lab.face.journal.overlay ? "true" : "false", lab.face.journal.offset, lab.face.journal.count, lab.face.status.open ? "true" : "false", lab.face.status.page);
        sim_emit_frame(); fflush(stdout);
}
static bool valid_event(int event) { return event >= LAB_TAP && event <= LAB_HOLD; }
static void event(int kind, int x, int y) {
    // The physical app closes the lab to its real Settings at this boundary.
    // The emulator has temporary settings only, so enter their native preview.
    if (kind == LAB_BOOT && lab.selected < 0) screen_lab_select(&lab, LAB_SETTINGS);
    else screen_lab_event(&lab, kind, x, y);
}
static void command(const char *line) {
    char name[24]; int a = 0, b = 0, c = 0;
    if (sscanf(line, "%23s %d %d %d", name, &a, &b, &c) < 1) return;
    if (!strcmp(name, "catalog")) {
        fputs("[", stdout);
        for (int i = 0; i < LAB_COUNT; i++) printf("%s\"%s\"", i ? "," : "", screen_lab_name(i));
        puts("]"); fflush(stdout);
    } else if (!strcmp(name, "select")) screen_lab_select(&lab, a);
    else if (!strcmp(name, "event") && valid_event(a)) event(a, b, c);
    else if (!strcmp(name, "pointer")) screen_lab_pointer(&lab, a != 0, b, c);
    else if (!strcmp(name, "tilt")) { tilt_x = a; tilt_y = b; }
    else if (!strcmp(name, "character")) { screen_lab_init(&lab, a == CHARACTER_PLUSH ? CHARACTER_PLUSH : CHARACTER_TESS); }
    else if (!strcmp(name, "frame")) {
        frame();
    }
}
int main(void) {
    sim_load_assets();
    screen_lab_init(&lab, CHARACTER_TESS);
    char line[160];
    while (fgets(line, sizeof line, stdin)) command(line);
    return 0;
}
