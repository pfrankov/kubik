// Native Skies entry for the single-contact S3 panel, with the C6 diagonal contrast.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tess_sky_play.h"

static face_t face;
static void ready(void) {
    tess_games_restore(&face, 0);
    memset(&face, 0, sizeof face);
    face.character = CHARACTER_TESS;
    face.mode = MODE_IDLE;
    face.tess_points_ready = true;
    face.rng = 12345;
    tess_games_restore(&face, 0);
    face.tess_games.self_wait = 999;
}
static void contacts(int x, int y, uint8_t id) {
    face.touch_in = (touch_frame_t){.point = {{.x=x, .y=y, .id=id, .down=true}}};
#if !CONFIG_IDF_TARGET_ESP32S3
    face.touch_in.point[1] = (touch_point_t){.x=479-x, .y=UI_H-1-y, .id=id+1, .down=true};
#endif
}
static void tick(unsigned count) {
    for (unsigned i = 0; i < count; i++) sky_summon_tick(&face, .0625f);
}
static void released(void) { face.touch_in = (touch_frame_t){0}; tick(1); }
static void no_entry(void) {
    tick(32);
    assert(face.tess_games.game != TESS_GAME_SKY && !face.tess_games.sky);
}
static void exact_threshold_and_every_corner(void) {
    const int positions[][2] = {{0,0},{99,99},{380,0},{479,99},{0,UI_H-100},{99,UI_H-1},{380,UI_H-100},{479,UI_H-1}};
    for (unsigned i = 0; i < sizeof positions / sizeof *positions; i++) {
        ready(); contacts(positions[i][0], positions[i][1], 4);
        tick(23); assert(face.tess_games.game == TESS_GAME_NONE && !face.tess_games.sky);
        tick(1); assert(face.tess_games.game == TESS_GAME_SKY && face.tess_games.sky);
        uint32_t round = face.tess_games.round;
        tick(48); assert(face.tess_games.round == round); // A held gesture never restarts an active sky.
    }
}
static void invalid_positions_and_contact_counts(void) {
    const int positions[][2] = {{100,0},{0,100},{379,0},{0,UI_H-101},{240,UI_CY},{-1,0},{0,-1},{480,0},{0,UI_H}};
    for (unsigned i = 0; i < sizeof positions / sizeof *positions; i++) {
        ready(); contacts(positions[i][0], positions[i][1], 2); no_entry();
    }
    ready(); contacts(20,20,2);
#if CONFIG_IDF_TARGET_ESP32S3
    face.touch_in.point[1] = (touch_point_t){.x=460,.y=460,.id=3,.down=true};
#else
    face.touch_in.point[1].down = false;
#endif
    no_entry();
}
static void movement_and_release_rearm(void) {
    ready(); contacts(20,20,2); tick(16);
    contacts(240,240,2); tick(1);
    contacts(20,20,2); no_entry(); // Once a held gesture leaves its corners, release is required.
    released(); contacts(20,20,2); tick(24);
    assert(face.tess_games.game == TESS_GAME_SKY);
    assert(tess_games_end_sky(&face));
    no_entry(); // Ending the game with the original finger held cannot relaunch it.
    released(); contacts(20,20,2); tick(24);
    assert(face.tess_games.game == TESS_GAME_SKY);
}
static void replacing_the_contact_restarts_the_clock(void) {
    ready(); contacts(20,20,2); tick(16);
    contacts(20,20,7); tick(16);
    assert(face.tess_games.game == TESS_GAME_NONE);
    tick(8); assert(face.tess_games.game == TESS_GAME_SKY);
    ready(); contacts(20,20,0); tick(16);
    released(); contacts(20,20,0); tick(23);
    assert(face.tess_games.game == TESS_GAME_NONE);
    tick(1); assert(face.tess_games.game == TESS_GAME_SKY);
}
static void short_touch_and_interrupted_stretch(void) {
    ready(); contacts(20,20,2); tick(1); released(); no_entry();
    ready(); face.tess_games.game = TESS_GAME_STRETCH;
    face.tess_games.pressed = true;
    contacts(20,20,2); tick(1);
    assert(face.tess_games.game == TESS_GAME_NONE && !face.tess_games.pressed && face.tess_games.finger_owned);
    tick(23); assert(face.tess_games.game == TESS_GAME_SKY);
}
#define BLOCKED(field, value) do { \
    ready(); contacts(20,20,2); tick(8); \
    face.field = (value); no_entry(); \
    face.field = original.field; no_entry(); \
    released(); contacts(20,20,2); tick(24); \
    assert(face.tess_games.game == TESS_GAME_SKY); \
} while (0)
static void unavailable_states_cancel_until_release(void) {
    ready(); const face_t original = face;
    BLOCKED(dark, true);
    BLOCKED(dimmed, true);
    BLOCKED(menu.open, true);
    BLOCKED(mode, MODE_SETUP);
    BLOCKED(mode, MODE_SPEAKING);
    BLOCKED(live_active, true);
    BLOCKED(card_n, 1);
    BLOCKED(tess_fallen, true);
    BLOCKED(tess_points_ready, false);
    BLOCKED(tess_hold[TR_SCATTER], 1);
    BLOCKED(tess_games.available, false);
    BLOCKED(character, CHARACTER_PLUSH);
}
static void either_slot_and_diagonal_contrast(void) {
    ready(); contacts(20,20,4);
#if CONFIG_IDF_TARGET_ESP32S3
    face.touch_in.point[1] = face.touch_in.point[0];
    face.touch_in.point[0].down = false;
    tick(24); assert(face.tess_games.game == TESS_GAME_SKY);
#else
    face.touch_in.point[1].x = 460; face.touch_in.point[1].y = 20;
    no_entry(); // Adjacent corners remain ordinary handling on C6.
    ready(); contacts(20,20,4);
    face.touch_in.point[1].id = 4;
    no_entry(); // One duplicate tracking ID is never a two-finger chord.
#endif
}
int main(void) {
    exact_threshold_and_every_corner();
    invalid_positions_and_contact_counts();
    movement_and_release_rearm();
    replacing_the_contact_restarts_the_clock();
    short_touch_and_interrupted_stretch();
    unavailable_states_cancel_until_release();
    either_slot_and_diagonal_contrast();
    ready(); // Return the last sky allocation to the real lifetime owner.
    puts(CONFIG_IDF_TARGET_ESP32S3 ? "S3 single-contact Skies: threshold, corners, identity, cancellation and rearm passed" :
                                   "C6 diagonal Skies: threshold, corners, identity, cancellation and rearm passed");
    return 0;
}
