#include "../main/screen_lab.h"
#include <assert.h>
#include <stdio.h>

static screen_lab_t lab;
static scene_t scene;
static void tap(int x, int y) { screen_lab_event(&lab, LAB_TAP, x, y); }
static void catalog_rows_and_footer(void) {
    screen_lab_select(&lab, -1);
    tap(350, UI_FOOTER_Y); assert(lab.page == 1);
    tap(240, 144); assert(lab.selected == LAB_SETUP_QR);
    screen_lab_select(&lab, -1); lab.page = 0;
    tap(240, 144 + 2 * LAB_ROW_H); assert(lab.selected == LAB_WIFI_QR);
    screen_lab_select(&lab, -1); lab.page = 0;
    tap(100, UI_FOOTER_Y); assert(lab.page == (LAB_COUNT + 2) / 3 - 1);
}
static void preview_controls(void) {
    screen_lab_select(&lab, LAB_RECORD);
    assert(screen_lab_control_y(&lab) == UI_FOOTER_Y);
    screen_lab_draw(&lab, &scene);
    tap(80, UI_FOOTER_Y); assert(!lab.signal);
    tap(400, UI_FOOTER_Y); assert(lab.selected == -1);
    screen_lab_select(&lab, LAB_LIVE);
    assert(screen_lab_control_y(&lab) == 92);
    screen_lab_dispose(&lab);
}
int main(void) {
    for (int character = CHARACTER_PLUSH; character <= CHARACTER_TESS; character++) {
        screen_lab_init(&lab, character);
        catalog_rows_and_footer();
        preview_controls();
    }
    printf("Screen Lab %dx%d: catalog rows, footer hits and both-character controls passed\n", UI_W, UI_H);
    return 0;
}
