// The play model alone: discoveries, touch routing, finger ownership and preemption. Her body, the burst
// and feedback are stubs (tools/test-tess-games.py); tess_games_integration_test.c runs the real face.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tess_internal.h"

static face_t f;
static void step(float seconds) { for (int i = 0; i < (int)(seconds * 50); i++) tess_games_update(&f, .02f); }
static void ready(uint8_t progress) {
    memset(&f, 0, sizeof f);
    f.character = CHARACTER_TESS;
    f.mode = MODE_IDLE;
    f.rng = 12345;
    f.tess_points_ready = true;
    tess_games_restore(&f, progress);
    f.tess_games.self_wait = 999;
}
static void finger(bool down, float x, float y) { f.rub_in = (rub_input_t){.down = down, .x = x, .y = y}; tess_games_update(&f, .02f); }
static unsigned waves(void) { unsigned n = 0; for (int w = 0; w < TESS_WAVES; w++) n += f.tess_games.wave[w].amp != 0; return n; }

static void restore_and_growth(void) {
    ready(0xff);
    assert(f.tess_games.progress == TESS_PROGRESS_MASK && f.tess_games.form == 3);
    ready(1); assert(f.tess_games.form == 1);
    ready(7); assert(f.tess_games.form == 2);
    ready(0); assert(f.tess_games.form == 0 && !tess_games_active(&f));
    uint32_t round = f.tess_games.round;
    tess_games_restore(&f, 0);
    assert(f.tess_games.round != round && f.tess_games.round);
}
static void taps_on_her_pluck_and_stay_ordinary(void) {
    ready(0);
    assert(!tess_games_event(&f, FEV_TAP, 250, 260));  // the tap's ordinary reactions still run
    assert(waves() == 1 && f.tess_games.progress == 1);
    assert(!tess_games_event(&f, FEV_TAP, 470, 20));    // far from her: no pluck
    assert(waves() == 1);
    tess_games_event(&f, FEV_WAKE, 0, 0);               // every tap wakes the app first: the wave keeps ringing
    tess_games_event(&f, FEV_TAP, 230, 250);
    assert(waves() == 2);
    step(3); assert(!waves());
    assert(!tess_games_event(&f, FEV_TAP, 250, 260));
    assert(f.tess_games.progress == 1);                 // a repeat discovers nothing new
}
static void a_finger_beside_her_owns_its_pull(void) {
    ready(0);
    finger(true, 400, 255);
    assert(tess_games_active(&f) && f.tess_games.game == TESS_GAME_STRETCH);
    for (int i = 0; i < 20; i++) finger(true, 440, 255);
    assert(f.tess_games.pull_k > .9f);
    assert(tess_games_event(&f, FEV_PET, 440, 255));   // a held pull is not petting
    finger(false, 440, 255);
    assert(f.tess_games.game == TESS_GAME_FLY && (f.tess_games.progress & 8));
    assert(tess_games_event(&f, FEV_TAP, 440, 255));   // its release never becomes an ordinary tap
    // A quick touch beside her is an ordinary tap.
    ready(0);
    finger(true, 400, 100); finger(false, 400, 100);
    assert(!tess_games_active(&f) && !tess_games_owns_tap(&f));
    assert(!tess_games_event(&f, FEV_TAP, 400, 100));
}
static void preemption_stops_everything(void) {
    ready(0);
    tess_games_event(&f, FEV_TAP, 250, 260);
    finger(true, 400, 255); step(.1f);
    f.card_n = 1; step(.02f);
    assert(!f.tess_games.game && !waves() && f.tess_games.pull_k == 0);
    f.card_n = 0;
    f.rub_in.down = false; step(.02f);
    assert(tess_games_owns_tap(&f) && tess_games_event(&f, FEV_TAP, 400, 255));
    assert(!tess_games_owns_tap(&f) && !tess_games_event(&f, FEV_TAP, 250, 260));
    ready(0);  // a release whose tap never arrives must not swallow the next ordinary tap
    finger(true, 400, 255); step(.1f);
    f.card_n = 1; step(.02f); f.card_n = 0;
    f.rub_in.down = false; step(.6f);
    assert(!tess_games_owns_tap(&f) && !tess_games_event(&f, FEV_TAP, 250, 260));
    ready(0);
    tess_games_event(&f, FEV_TAP, 250, 260);
    tess_games_event(&f, FEV_NOTIFY, 0, 0);
    assert(!waves());
    tess_games_event(&f, FEV_TAP, 250, 260);
    tess_games_set_available(&f, false);
    assert(!waves() && !tess_games_active(&f));
}
static void alone_she_plays_only_when_quiet(void) {
    ready(0);
    f.tess_games.self_wait = .1f;
    step(.2f);
    assert(f.tess_games.self_spinning || waves());
    ready(0);
    f.tess_games.self_wait = .1f;
    f.rub_in.down = true;
    step(.3f);
    assert(!f.tess_games.self_spinning && !waves());
}
int main(void) {
    restore_and_growth();
    taps_on_her_pluck_and_stay_ordinary();
    a_finger_beside_her_owns_its_pull();
    preemption_stops_everything();
    alone_she_plays_only_when_quiet();
    puts("native Tess play: plucks, pulls, ownership, preemption and self-play rules ok");
}
