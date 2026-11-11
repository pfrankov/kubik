#include <assert.h>
#include <stdio.h>
#include "app_state.h"
static bool s_menu, s_setup, s_pair_hidden, lab, locked;
static char s_pair_code[17];
static power_state_t s_power=PWR_AWAKE;
typedef struct { bool open; } page_t;
static struct { page_t printer,agent,status,journal; struct {bool sound;} menu; } g_face;
static bool app_lab_active(void) { return lab; }
static void face_lock(void) { assert(!locked);locked=true; }
static void face_unlock(void) { assert(locked);locked=false; }
/* PRODUCTION_VISIBILITY */
static void hidden_views(void) {
    g_face.agent.open=true;assert(!app_printer_visible());g_face.agent.open=false;
    g_face.status.open=true;assert(!app_printer_visible());g_face.status.open=false;
    g_face.journal.open=true;assert(!app_printer_visible());g_face.journal.open=false;
    g_face.menu.sound=true;assert(!app_printer_visible());g_face.menu.sound=false;
    assert(app_printer_visible()); // BOOT/back restored the ordinary Settings page.
}
static void physical_visibility(void) {
    lab=true;assert(!app_printer_visible());lab=false;
    s_power=PWR_DARK;assert(!app_printer_visible());s_power=PWR_DARK_MANUAL;assert(!app_printer_visible());
    s_power=PWR_DIMMED;assert(app_printer_visible());s_power=PWR_AWAKE;
    s_setup=true;assert(!app_printer_visible());s_setup=false;
    s_pair_code[0]='1';assert(!app_printer_visible());s_pair_code[0]=0;
}
int main(void) {
    assert(!app_printer_visible());s_menu=true;assert(app_printer_visible());hidden_views();
    g_face.printer.open=true;g_face.agent.open=true;assert(app_printer_visible());
    g_face.printer.open=false;assert(!app_printer_visible());g_face.agent.open=false;
    physical_visibility();assert(!locked);
    puts("printer visibility: real Settings/details only; dark, Screen Lab and other pages do not poll");
}
