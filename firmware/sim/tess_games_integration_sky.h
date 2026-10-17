// Skies through the real face (included by tess_games_integration_test.c, which owns the harness): the summoning star,
// relays and the aimed source under a finger, tools that click into place, and each sky's own ending melody.
static void finger_at(float x, float y, int frames) { for (int i = 0; i < frames; i++) finger(true, x, y); }

static void the_summoning_star_wanders_and_a_tap_on_it_answers(void) {
    float x0, y0, x, y;
    invite(); assert(tess_sky_calling(&face, &x0, &y0));
    run(1.5f); assert(tess_sky_calling(&face, &x, &y) && hypotf(x - x0, y - y0) > 40);   // it wanders round her
    tap(x + 30, y - 25);
    assert(face.tess_games.game == TESS_GAME_SKY && sky() && sky()->phase == SKY_UNRAVEL);
}

// A sky of one relay star with a string, an aimed source, all set by hand on the sky that is playing.
static sky_world_t *hand_made_sky(void) {
    face.tess_games.sky_elements = 6;
    answer(); until_phase(SKY_PLAY, 6);
    sky_world_t *w = &sky()->w;
    w->count_stars = 1; w->count_tools = 1;
    w->star[0] = (sky_star_t){.x = 240, .y = 255, .ax = 240, .ay = 255, .relay = true, .dir = 0, .sdir = 2, .hz = 440};
    w->tool[0] = (sky_tool_t){.kind = TOOL_STRING, .movable = 1, .x = 150, .y = 380, .dx = 50, .dy = 0, .sx = 330, .sy = 380, .sdx = 50, .sdy = 0};
    w->source = (sky_source_t){.x = 100, .y = 120, .dx = 1, .dy = 0, .speed = 120, .sdx = .6f, .sdy = .8f, .aim = true};
    sky()->hint_tool = -1;
    return w;
}
static void a_tap_turns_a_relay_and_tools_click_into_place(void) {
    sky_world_t *w = hand_made_sky();
    tones_heard = 0;
    finger_at(242, 252, 3); finger(false, 242, 252);
    assert(w->star[0].dir == 1);                          // a quick tap: its next way
    finger_at(240, 255, 20); finger(false, 240, 255);
    assert(w->star[0].dir == 1);                          // a long press is not a tap
    finger_at(150, 380, 2); finger_at(250, 380, 2); assert(!w->tool[0].snapped && w->tool[0].x == 250);
    finger_at(318, 386, 2); assert(w->tool[0].snapped && w->tool[0].x == 330 && w->tool[0].y == 380);   // near: it clicks there
    finger_at(340, 390, 2); assert(w->tool[0].snapped && w->tool[0].x == 330);                          // and holds
    finger_at(380, 390, 2); finger(false, 380, 390); assert(!w->tool[0].snapped && w->tool[0].x == 380);   // pulled away: free
    finger_at(100, 120, 2); finger_at(220, 140, 3); assert(w->source.dx > .98f);            // the source follows the finger
    finger_at(100 + 60, 120 + 82, 3); finger(false, 160, 202);
    assert(w->source.dx == w->source.sdx && w->source.dy == w->source.sdy);              // and clicks onto its way
    w->tool[0].x = 330; w->tool[0].y = 380;
    sky()->struggle = 999; run(.2f);                      // a long struggle: the tools in place, the wrong relay trembles
    assert(sky()->hint_tool == SKY_HINT_RELAY(0));
    assert(tones_heard & (1u << TT_SKY_PLUCK));           // the turn and the clicks are heard
    face_card(&face, "end"); frame(); assert(!sky());
}
// Solved, the sky sings back how it was played: every star's note in the order lit, its star flashing, then its chord.
static void every_sky_sings_its_own_melody(void) {
    answer(); until_phase(SKY_PLAY, 4);
    const sky_star_t *star = &sky()->w.star[0];
    for (int i = 0; i < 40 * 30 && sky()->phase == SKY_PLAY; i++) finger(true, star->x, star->y);
    finger(false, 0, 0);
    assert(sky()->phase == SKY_RESOLVE && sky()->melody.n >= 6);
    assert(sky()->played.n == 1 && sky()->played.order[0] == 0 && sky()->melody.tour[0] == 0);   // sung as it was played
    float len = sky()->resolve_len, glowed = 0;
    assert(len > SKY_MELODY_AT_S + 2.2f && len < SKY_MELODY_AT_S + 4.3f + SKY_CHORD_RING_S);
    tones_heard = 0;
    for (int i = 0; i < (int)(len * 30) - 2 && sky()->phase == SKY_RESOLVE; i++) { frame(); glowed = fmaxf(glowed, sky()->w.star[0].glow); }
    assert(sky()->melody_next == sky()->melody.n && sky()->tour_lit == 1 && glowed > .9f);
    assert((tones_heard & (1u << TT_SKY_PLUCK)) && (tones_heard & (1u << TT_SKY_CHIME)));
    face_card(&face, "end"); frame(); assert(!sky());
}
static void skies_summon_play_and_sing(void) {
    the_summoning_star_wanders_and_a_tap_on_it_answers();
    a_tap_turns_a_relay_and_tools_click_into_place();
    every_sky_sings_its_own_melody();
}

static void pouring_uses_gravity_without_rest_pose_drift(void) {
    answer(); until_phase(SKY_PLAY, 4);
    tess_sky_t *k = sky();
    k->w.kind = SKY_POUR;
    face.grav_x = .3f; face.grav_y = -.2f;
    face.tess_games.tilt[0] = -.6f; face.tess_games.tilt[1] = .8f;
    sky_world_t expected = k->w;
    sky_particle_t particles[SKY_PARTICLES];
    memcpy(particles, k->p, sizeof particles);
    sky_force_t force = {.gx = face.grav_x, .gy = face.grav_y};
    sky_stream_step(&expected, particles, SKY_PARTICLES, 1.f / 30, &force, NULL);
    tess_sky_update(&face, 1.f / 30);
    assert(!memcmp(particles, k->p, sizeof particles));
    assert(k->calm == 0);
    face_card(&face, "end"); frame();
}

static void quiet_releases_survive_a_full_queue(void) {
    fresh(63);
    face.tess_play.tone_n = 0;
    for (int i = 0; i < TESS_TONES; i++) tess_tone(&face, TT_SKY_PLUCK, 440, .5f, 0);
    assert(face.tess_play.tone_n == TESS_TONES);
    face.dimmed = true;
    tess_tone_slot(&face, TT_SKY_PAD, 0, 440, 0, 0, 0);
    tess_tone_slot(&face, TT_SKY_HUM, 1, 220, 0, 0, 0);
    tess_tone_slot(&face, TT_SKY_PAD, 0, 440, 0, 0, 0);
    tess_tone_t tone;
    unsigned releases = 0;
    while (face_take_tone(&face, &tone)) { assert(tone.gain == 0); releases++; }
    assert(releases == 2 && !face.tess_play.tone_n);
    face.dark = true;
    tess_tone_slot(&face, TT_SKY_PAD, 2, 440, 0, 0, 0);
    assert(face_take_tone(&face, &tone) && tone.gain == 0 && tone.slot == 2);
    assert(!face_take_tone(&face, &tone));
}
