#include "screen_lab.h"
#include "ui_theme.h"
#include <stdio.h>

static void button(scene_t *s, const char *text, int x, int y, int width, uint32_t accent) {
    sc_rbox(s, x, y, width / 2.f, 32, 24, 0, UI_SURFACE, 1);
    sc_label(s, TXT_ACTION, text, x, y, 0, accent, 1);
}
static void catalog(screen_lab_t *lab, scene_t *s) {
    scene_begin(s, 0);
    sc_label(s, TXT_ACTION, "Screen Lab", 28, 36, -1, UI_TEXT, 1);
    sc_label(s, TXT_CAPTION, "Preview only / BOOT exits", 240, 78, 0, UI_MUTED, 1);
    for (int i = 0; i < 3; i++) {
        int index = lab->page * 3 + i;
        if (index >= LAB_COUNT) break;
        button(s, screen_lab_name(index), 240, 144 + i * 88, 440, i == 1 ? UI_GOLD : UI_ACCENT);
    }
    button(s, "Previous", 124, 428, 208, UI_MUTED);
    button(s, "Next", 356, 428, 208, UI_GOLD);
    char page[16]; snprintf(page, sizeof page, "%d / %d", lab->page + 1, (LAB_COUNT + 2) / 3);
    sc_label(s, TXT_CAPTION, page, 240, 382, 0, UI_MUTED, 1);
}
void screen_lab_draw(screen_lab_t *lab, scene_t *s) {
    if (lab->face.dark) { scene_begin(s, 0); return; }
    if (lab->selected < 0) { catalog(lab, s); return; }
    face_draw(&lab->face, s);
    if (screen_lab_controls(lab) && lab->overlay) {
        int y = screen_lab_control_y(lab);
        button(s, lab->signal ? "Pause" : "Signal", 80, y, 148, UI_ACCENT);
        button(s, "Next", 240, y, 148, UI_GOLD);
        button(s, "Done", 400, y, 148, UI_MUTED);
    }
}
