// Tess's play through the real face, burst, springs and renderer: each behaviour is reached by ordinary
// touch and is visible as her own dots moving, never as anything drawn on top of her.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_internal.h"
#include "../main/tess_games_internal.h"
#include "../main/tess_sky_play.h"
static face_t face;
static scene_t scene;
static unsigned most_dots, tones_heard;  // tones_heard: one bit per tone kind the speaker was given
static float ring_low, ring_high;        // the pitches her plucks rang at
static void frame(void) {
    face_update(&face, 1.f / 30); face_draw(&face, &scene);
    if ((unsigned)scene.dots_n > most_dots) most_dots = (unsigned)scene.dots_n;
    tess_tone_t tone;
    while (face_take_tone(&face, &tone)) { assert(tone.hz >= 40 && tone.hz <= 3000 && (tone.gain > 0 || tone.kind == TT_SKY_PAD || tone.kind == TT_SKY_HUM) && tone.gain <= 1); tones_heard |= 1u << tone.kind;
        if (tone.kind == TT_RING) { ring_low = fminf(ring_low, tone.hz); ring_high = fmaxf(ring_high, tone.hz); } }
}
static void run(float seconds) { for (int i=0;i<(int)(seconds*30);i++) frame(); }
static void fresh(unsigned progress) {
    tess_games_set_available(&face, false);  // Release the preceding fixture through the real exit path.
    face_init(&face);
    face_set_character(&face, CHARACTER_TESS);
    tess_games_restore(&face, progress);
    face_set_mode(&face, MODE_IDLE);
    run(3);
    face.tess_games.self_wait = 99;  // no self-play unless a test waits for it
}
static void tap(float x, float y) { face_event(&face, FEV_TAP, x, y); }
static void finger(bool down, float x, float y) { face.rub_in=(rub_input_t){.down=down,.x=x,.y=y}; frame(); }
static void touch(float x, float y) { finger(true,x,y); finger(true,x,y); finger(false,x,y); tap(x,y); }
static bool found(unsigned bit) { return face.tess_games.progress & (1u << bit); }
static unsigned waves(void) { unsigned n=0; for (int w=0;w<TESS_WAVES;w++) n+=face.tess_games.wave[w].amp!=0; return n; }
// How far her dots reach from the panel centre, and towards a side.
static float extent(void) {
    tess_projected_t dots[TESS_N]; tess_project_dots(&face,dots);
    float far=0; for (int i=0;i<TESS_N;i++) far=fmaxf(far,hypotf(dots[i].x/8.f-240,dots[i].y/8.f-255));
    return far;
}
static float reach_x(void) {
    tess_projected_t dots[TESS_N]; tess_project_dots(&face,dots);
    float far=0; for (int i=0;i<TESS_N;i++) far=fmaxf(far,dots[i].x/8.f);
    return far;
}
static void a_tap_plucks_a_wave_through_her_edges(void) {
    fresh(0);
    float rest=extent();
    face.tess_play.cue_n=0;
    touch(300,255);
    assert(waves()==1 && found(0));
    assert(!tess_games_active(&face));  // a pluck is not a round: ordinary tap reactions still ran
    bool shaped=false; float pulse=0, moved=0;
    for (int i=0;i<20;i++) { frame(); shaped=shaped||face.tess_games.shaping; pulse=fmaxf(pulse,face.tess_games.pulse); moved=fmaxf(moved,fabsf(extent()-rest)); }
    assert(shaped && pulse>.4f && moved>1);
    run(2);
    assert(!face.tess_games.shaping && !waves());
}
// Plucking corner after corner never plays a tune: her whole body rings at one pitch, the corner only
// changes which overtone sounds over it.
static void plucked_corners_ring_one_body_not_a_scale(void) {
    fresh(0);
    ring_low = 1e9f; ring_high = 0;
    const float corners[][2] = {{300,255},{180,200},{240,330},{320,180},{170,300}};
    for (int i = 0; i < 5; i++) { touch(corners[i][0], corners[i][1]); run(1.5f); }
    assert(ring_high > 0 && ring_high / ring_low < 1.01f);
}
static void two_quick_plucks_on_different_sides_echo(void) {
    fresh(0);
    touch(175,200); run(.1f); touch(305,310); touch(240,300);
    assert(found(0) && found(1) && waves()==3);  // quick plucks ring together, none cuts another short
}
static void swipes_wind_her_up_until_she_blooms(void) {
    fresh(0);
    float rest=extent(), widest=0;
    for (int s=0;s<6 && face.tess_games.bloom<.8f;s++) {
        for (int i=0;i<4;i++) finger(true,170+i*45,255);
        finger(false,350,255);
    }
    assert(hypotf(face.tess_mood.drag_speed[0],face.tess_mood.drag_speed[1])>10);
    for (int i=0;i<10;i++) { frame(); widest=fmaxf(widest,extent()); }
    assert(found(2) && widest>rest+15);  // she flowers open
    run(4);
    assert(face.tess_games.bloom<.2f);
}
static void a_finger_beside_her_stretches_and_twangs(void) {
    fresh(0);
    float rest=reach_x();
    finger(true,375,255);
    assert(tess_games_active(&face) && face.tess_games.game==TESS_GAME_STRETCH);
    for (int i=0;i<15;i++) finger(true,385,255);  // held there
    assert(reach_x()>rest+25);       // a corner and its edges reach for the finger
    finger(false,385,255); tap(385,255);
    assert(!tess_games_active(&face) && found(3) && face.tess_taps==0);
    run(1.5f); assert(!face.tess_games.shaping && face.tess_games.pull_k==0);  // (the discovery grew her: no old extent)
    // A quick touch beside her that never pulled is an ordinary tap.
    finger(true,400,100); finger(false,400,100);
    assert(!tess_games_active(&face) && !face.tess_games.finger_owned);
}
static void a_far_pull_slings_her_into_the_rim(void) {
    fresh(0);
    finger(true,400,255);
    for (int x=400;x<=440;x+=10) finger(true,x,255);
    for (int i=0;i<8;i++) finger(true,440,255);
    finger(false,440,255); tap(440,255);
    assert(face.tess_games.game==TESS_GAME_FLY && face.tess_games.fly_v[0]<-5);
    bool bounced=false; float leftmost=0;
    for (int i=0;i<60 && face.tess_games.game==TESS_GAME_FLY;i++) {
        frame(); bounced=bounced||face.tess_games.squash_k>.1f; leftmost=fminf(leftmost,face.tess_games.offset[0]);
    }
    assert(bounced && found(4) && leftmost<-1.2f && face.tess_hold[TR_SCATTER]<=0);  // a medium throw only bounces
    run(4); assert(!tess_games_active(&face) && fabsf(face.tess_games.offset[0])<.05f);
}
static void a_tap_catches_her_in_flight(void) {
    fresh(0);
    assert(tess_games_start(&face,TESS_PLAY_FLY,0)); run(.1f);
    touch(face.tess_games.hit[0],face.tess_games.hit[1]);
    assert(face.tess_games.game==TESS_GAME_FLY && hypotf(face.tess_games.fly_v[0],face.tess_games.fly_v[1])<2.5f);
}
static void the_hardest_throw_shatters_her_and_a_sweep_gathers_her(void) {
    fresh(0);
    finger(true,400,255);
    for (int x=400;x<=470;x+=10) finger(true,x,255);
    for (int i=0;i<8;i++) finger(true,470,255);
    finger(false,470,255);
    for (int i=0;i<30 && face.tess_hold[TR_SCATTER]<=0;i++) frame();
    assert(face.tess_hold[TR_SCATTER]>0);
    run(1.f);
    finger(true,20,20);
    assert(face.tess_games.game==TESS_GAME_GATHER);
    for (float y=20;y<=470 && face.tess_games.phase==TESS_GAME_WAIT;y+=60)
        for (float x=20;x<=460;x+=40) finger(true,(int)y/60%2?480-x:x,y);
    finger(false,240,255); tap(240,255);
    assert(found(5) && face.tess_hold[TR_SCATTER]<=0);
    run(3); assert(!tess_games_active(&face));
}
static void rapid_taps_still_burst_and_the_burst_returns_alone(void) {
    fresh(0);
    for (int i=0;i<6;i++) { tap(240,255); run(.25f); }
    assert(face.tess_hold[TR_SCATTER]>0 && !tess_games_active(&face));
    run(5); assert(face.tess_hold[TR_SCATTER]<=0 && face.tess_reaction[TR_SCATTER]==0);
}
static void a_drag_on_her_still_turns_her(void) {
    fresh(0);
    float before=face.tess_mood.drag_angle[1];
    finger(true,240,255);
    for (int i=1;i<=4;i++) finger(true,240,255+i*20);
    assert(!tess_games_active(&face) && fabsf(face.tess_mood.drag_angle[1]-before)>.3f);
    finger(false,240,335);
}
static unsigned self_kinds(const tess_games_t *g) {  // one bit per act of her own play seen
    unsigned kinds=g->self_act?1u<<(g->self_act-1):0;
    for (int w=0;w<TESS_WAVES;w++) if (g->wave[w].amp!=0 && g->wave[w].quiet) kinds|=1;
    if (g->bloom>.1f) kinds|=4;
    return kinds;
}
static void alone_she_plays_with_herself(void) {
    fresh(0);
    face.tess_games.self_wait=.1f;
    unsigned kinds=0;
    for (int i=0;i<4*30*30 && kinds!=15;i++) { frame(); kinds|=self_kinds(&face.tess_games); }  // up to two minutes of quiet
    assert(kinds&1 && kinds&2 && kinds&8);  // alone: she plucks herself, reaches out and wrestles
    assert(!face.tess_games.progress);  // her own play discovers nothing for you
    // Still excited from play, she soon spins or plucks herself, asking for more.
    face.tess_games.excite=.9f; face.tess_games.self_wait=.1f;
    bool lively=false;
    for (int i=0;i<30*12 && !lively;i++) { frame(); lively=face.tess_games.bloom>.1f||face.tess_games.wave[0].amp!=0||face.tess_games.wave[1].amp!=0; }
    assert(lively);
}
static void priority_stops_play_at_once(void) {
    fresh(1);
    finger(true,400,255); for (int i=0;i<10;i++) finger(true,430,255);
    face_card(&face,"An incoming agent reply"); frame();
    assert(!face.tess_games.game && face.tess_games.pull_k==0);
    finger(false,430,255); tap(430,255);
    assert(face.card_n>0);  // the stretching finger's release does not dismiss the reply
    fresh(1); touch(300,255); face.menu.open=true; frame();
    assert(!waves());
    face.menu.open=false;
    fresh(1); assert(tess_games_start(&face,TESS_PLAY_FLY,0)); run(.1f);
    face.dark=true; frame();
    assert(!face.tess_games.game);
    face.dark=false; run(6); assert(fabsf(face.tess_games.offset[0])<.1f);  // home, sloshing to rest on her spring
    fresh(0); touch(300,255); face_event(&face,FEV_NOTIFY,0,0);
    assert(!waves());
}
static void invalid_taps_never_poison_native_touch_state(void) {
    const float invalid[][2]={{NAN,255},{240,INFINITY},{-1,255},{240,480}};
    fresh(0);
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
        tap(invalid[i][0],invalid[i][1]);
        assert(isfinite(face.tess_look_to[0]) && isfinite(face.tess_mood.amp) && isfinite(face.tess_touch_x));
        run(.3f);
        assert(!tess_games_active(&face) && !face.tess_games.progress);
    }
}
static void an_upright_device_is_her_level(void) {
    face_init(&face); face_set_character(&face, CHARACTER_TESS);
    face.grav_y=.8f;  // the device stands upright from the start
    tess_games_restore(&face, 0); face_set_mode(&face, MODE_IDLE);
    run(3);
    assert(fabsf(face.tess_games.offset[1])<.05f);  // she does not roll to the bottom: that is how it stands
    face.grav_y=0;
}
static void tilting_rolls_her_against_the_rim_and_home(void) {
    fresh(0); tones_heard=0;
    face.grav_x=.6f;
    float far=0; bool hit=false;
    for (int i=0;i<60;i++) { frame(); far=fmaxf(far,face.tess_games.offset[0]); hit=hit||face.tess_games.squash_k>.05f; }
    assert(far>1.3f && hit && (tones_heard & (1u<<TT_THUD)));  // she rolled downhill into the rim and thudded
    face.grav_x=-.6f;
    float other=0;
    for (int i=0;i<60;i++) { frame(); other=fminf(other,face.tess_games.offset[0]); }
    assert(other<-1.f);  // tilted back, she rolls across
    face.grav_x=0; run(4);
    assert(fabsf(face.tess_games.offset[0])<.1f);  // level again: home
}
// A burst's spread: how irregular its nearest-neighbour spacing is (0 a perfect grid; clumps push it past
// .45), how close the closest pair is, and the biggest hole (the farthest any point of the glass is from a dot).
// And how many dots were pressed flat against the glass's edge (16 px in from it), lined up there.
typedef struct { float spread, closest, hole; int edge; } layout_t;
static layout_t burst_layout(float px[TESS_N][2]) {
    layout_t out={0,1e9f,0,0};
    for (int k=0;k<TESS_N;k++) {
        tess_scatter_spot_px(k,&px[k][0],&px[k][1]);
        out.edge+=fabsf(px[k][0]-240)>=223.f || fabsf(px[k][1]-240)>=223.f;
    }
    float mean=0, sq=0;
    for (int k=0;k<TESS_N;k++) {
        float near=1e9f;
        for (int j=0;j<TESS_N;j++) if (j!=k) near=fminf(near,hypotf(px[k][0]-px[j][0],px[k][1]-px[j][1]));
        mean+=near; sq+=near*near; out.closest=fminf(out.closest,near);
    }
    mean/=TESS_N; out.spread=sqrtf(fmaxf(0,sq/TESS_N-mean*mean))/mean;
    for (int y=24;y<=456;y+=12) for (int x=24;x<=456;x+=12) {
        float near=1e9f;
        for (int k=0;k<TESS_N;k++) near=fminf(near,hypotf(px[k][0]-x,px[k][1]-y));
        out.hole=fmaxf(out.hole,near);
    }
    return out;
}
static void every_burst_is_its_own_organic_spread(void) {
    static float a[TESS_N][2], b[TESS_N][2];
    fresh(0);
    for (int burst=0;burst<6;burst++) {
        face.tess_games.fly_v[0]=burst%3==1?8.f:0; face.tess_games.fly_v[1]=burst%3==2?-8.f:0;  // some thrown, a way each
        tess_scatter_begin(&face); layout_t l=burst_layout(burst%2?b:a);
        face.tess_games.fly_v[0]=face.tess_games.fly_v[1]=0;
        printf("burst %d: spacing spread %.2f, closest %.1f px, biggest hole %.1f px, %d on the edge\n",burst,l.spread,l.closest,l.hole,l.edge);
        assert(l.spread>.1f && l.spread<.36f);  // irregular, not a grid; no clumps
        assert(l.closest>=10.f);                // nothing on top of anything (dots are a few px wide)
        assert(l.hole<70.f);                    // no big empty areas
        assert(l.edge<=5);                      // a throw packs them its way, never into a row along the edge
        run(5);
    }
    float moved=0; for (int k=0;k<TESS_N;k++) moved+=hypotf(a[k][0]-b[k][0],a[k][1]-b[k][1]);
    assert(moved/TESS_N>40);  // and each burst its own
}
static void tilting_the_burst_pours_her_dots_home(void) {
    fresh(0);
    for (int i=0;i<6;i++) { tap(240,255); run(.25f); }
    run(.8f);
    unsigned home=0;
    for (int swing=0;swing<6 && face.tess_games.phase!=TESS_GAME_CELEBRATE;swing++) {
        face.grav_x=swing%2?-.7f:.7f; face.grav_y=swing%3==0?.5f:-.4f;
        for (int i=0;i<30;i++) frame();
    }
    home=face.tess_games.streak;
    assert(face.tess_games.game==TESS_GAME_GATHER && home>5);  // dots slid through her core and stayed
    face.grav_x=face.grav_y=0; run(8);
}
static void her_temper_changes_how_she_answers(void) {
    fresh(1); tones_heard=0;
    bool recoiled=false, giggled=false;
    for (int i=0;i<10;i++) {
        touch(300,255); frame();
        for (int w=0;w<TESS_WAVES;w++) { recoiled=recoiled||face.tess_games.wave[w].amp<0; giggled=giggled||(face.tess_games.wave[w].amp>0&&face.tess_games.wave[w].shell_s<.09f); }
        run(.2f);
    }
    assert(giggled && recoiled && (tones_heard & (1u<<TT_RING)) && (tones_heard & (1u<<TT_CHIRP)));
    run(12); assert(face.tess_games.annoy<.4f);  // and it passes
    fresh(1); face.tess_games.fear=.9f;
    float rest=reach_x();
    finger(true,380,255); for (int i=0;i<15;i++) finger(true,390,255);
    assert(reach_x()<rest+12);  // scared, she shies from the pull instead of reaching for it
    finger(false,390,255);
}
static void a_knock_rattles_her(void) {
    fresh(0); tones_heard=0;
    face.jolt_dvx=.06f; frame();
    assert(waves()>=1);  // corners jiggle
    run(.3f); assert(tones_heard & (1u<<TT_GRAIN));
}
static void her_movements_sound_like_themselves(void) {
    fresh(0); tones_heard=0;
    finger(true,400,255); for (int x=400;x<=440;x+=10) finger(true,x,255);
    for (int i=0;i<8;i++) finger(true,440,255);
    finger(false,440,255); run(2);
    assert(tones_heard & (1u<<TT_CREAK)); assert(tones_heard & (1u<<TT_TWANG));
    assert(tones_heard & (1u<<TT_WHOOSH)); assert(tones_heard & (1u<<TT_THUD));
}
static void background_events_preserve_a_round(void) {
    fresh(0);
    assert(tess_games_start(&face, TESS_PLAY_FLY, 0));
    uint32_t round = face.tess_games.round;
    face_event(&face, FEV_WAKE, 0, 0);
    assert(face.tess_games.game == TESS_GAME_FLY && face.tess_games.round == round);
    face_event(&face, FEV_SHAKE, 0, 0);
    assert(face.tess_games.game == TESS_GAME_FLY && face.tess_games.round == round);
    assert(face.tess_hold[TR_SCATTER] > 0);
    fresh(0);
    finger(true, 400, 255);
    assert(face.tess_games.game == TESS_GAME_STRETCH);
    face.tess_games.pull_k = .6f;
    face_event(&face, FEV_VOLUME, 50, 0);
    assert(face.tess_games.game == TESS_GAME_STRETCH && face.tess_games.pull_k == .6f);
    face_event(&face, FEV_NOTIFY, 0, 0);
    assert(!tess_games_active(&face));
}
static void visible_corners_are_the_touch_targets(void) {
    fresh(0);
    face_set_emotion(&face, EMO_CONFUSED, 3);
    face.view_q[0] = cosf(.3f); face.view_q[3] = sinf(.3f);
    run(.5f);
    tess_projected_t dots[TESS_N];
    tess_project_dots(&face, dots);
    for (int v = 0; v < 16; v++) {
        int slot = tess_play_vertex_slot(v), point = -1;
        for (int i = 0; i < TESS_N; i++) if (face.tess_form_of[i] == slot) point = i;
        assert(point >= 0);
        assert(fabsf(face.tess_games.vertex_px[v][0] - dots[point].x / 8.f) < .01f);
        assert(fabsf(face.tess_games.vertex_px[v][1] - dots[point].y / 8.f) < .01f);
    }
    int v = tess_play_nearest_vertex(&face, 300, 255, NULL);
    float x = face.tess_games.vertex_px[v][0], y = face.tess_games.vertex_px[v][1];
    tap(x, y);
    assert(face.tess_games.wave[0].origin == v);
}
static void a_rolled_corner_pulls_towards_the_screen_finger(void) {
    for (int rolled = 0; rolled < 2; rolled++) {
        face_t f = {0}; float points[TESS_N][3] = {{0}};
        tess_games_t *g = &f.tess_games;
        g->ready = true; g->form = 3; g->pull_k = 1;
        g->pull[0] = GAME_CX + TESS_PX_PER_UNIT; g->pull[1] = GAME_CY;
        float angle = rolled ? .38f : 0, pc = cosf(angle), ps = sinf(angle);
        tess_play_frame(&f, points, 1, 0, 0, pc, ps);
        float x = 0, y = 0, z = 0;
        tess_play_point(tess_play_vertex_slot(0), &x, &y, &z);
        assert(fabsf(x * pc - y * ps - 1) < .001f);
        assert(fabsf(x * ps + y * pc) < .001f);
    }
}
static tess_sky_t *sky(void) { return face.tess_games.sky; }
static void invite(void) {
    fresh(TESS_PROGRESS_MASK);
    for (int n = 0; n < 32 && !tess_sky_invite(&face); n++) {}
    assert(face.tess_games.sky_invite_t > 0);
}
static void answer(void) {
    invite(); run(1.2f);
    const float *v = face.tess_games.sky_call;
    tap(v[0] - 30, v[1] + 20);
    assert(face.tess_games.game == TESS_GAME_SKY && sky() && sky()->phase == SKY_UNRAVEL);
}
static void until_phase(sky_phase_t phase, float seconds) {
    for (int i = 0; i < (int)(seconds * 30) && sky() && sky()->phase != phase; i++) frame();
    assert(sky() && sky()->phase == phase);
}
static void an_ignored_invitation_asks_nothing(void) {
    float x, y; invite(); run(1); assert(tess_sky_calling(&face, &x, &y));
    tap(480 - x, 510 - y);   // a tap away from the star is only a tap
    tap(240, 255);
    assert(face.tess_games.game != TESS_GAME_SKY && !sky());
    run(6.5f);
    assert(face.tess_games.sky_invite_t == 0 && !tess_sky_calling(&face, &x, &y) && face.tess_games.game != TESS_GAME_SKY && !sky());
    tap(x, y);
    assert(face.tess_games.game != TESS_GAME_SKY);   // and where it was, once it has gone, starts nothing
}
// A rocking swing: the pull on the glass goes to one side of level, then the other, within a second or so.
static void rock_cube(float lean, int swings, float wait_s) {
    for (int i = 0; i < swings; i++) {
        face.grav_x = i % 2 ? -lean : lean;
        run(wait_s);
    }
    face.grav_x = 0;
}
static void she_asks_only_when_lying_down_and_a_rock_summons_her(void) {
    fresh(TESS_PROGRESS_MASK); run(2);
    face.grav_x = 0; face.grav_y = 1.f;   // standing upright: she never asks, however often
    int asked = 0;
    for (int i = 0; i < 200; i++) asked += tess_sky_invite(&face);
    assert(!asked && !face.tess_games.sky_invite_t);
    face.grav_y = .25f;                   // a slight tilt of the flat cube
    for (int i = 0; i < 200 && !asked; i++) asked += tess_sky_invite(&face);
    assert(asked && face.tess_games.sky_invite_t > 0);
    fresh(TESS_PROGRESS_MASK); run(2);
    face.grav_y = 1.f; rock_cube(.3f, 4, .3f); run(.5f);   // rocked while standing: nothing
    face.grav_y = .1f; rock_cube(.3f, 2, .3f); run(1.6f);  // two swings are not yet a rock
    assert(!face.tess_games.sky_invite_t);
    rock_cube(.3f, 3, 2.f); run(.5f);                      // slow swings are not a rock either
    assert(!face.tess_games.sky_invite_t);
    face.grav_y = .1f; rock_cube(.3f, 3, .3f);             // three quick swings summon, and only summon
    assert(face.tess_games.sky_invite_t > 0 && face.tess_games.game != TESS_GAME_SKY && !sky());
    face.grav_y = 0;
}
static void raising_the_cube_answers_an_invitation_like_a_touch(void) {
    fresh(TESS_PROGRESS_MASK); face.grav_x = 0; face.grav_y = .1f; run(1);
    invite(); face.grav_y = .6f; run(.5f);                 // tipped, not raised
    assert(face.tess_games.game != TESS_GAME_SKY && face.tess_games.sky_invite_t > 0);
    face.grav_y = 1.f; frame();                            // raised
    assert(face.tess_games.game == TESS_GAME_SKY && sky() && sky()->phase == SKY_UNRAVEL);
    fresh(TESS_PROGRESS_MASK); face.grav_y = 1.f; run(1);  // raised with no invitation: nothing
    face.grav_y = .1f; run(.5f); invite(); run(7.5f); face.grav_y = 1.f; run(.5f);   // or after it has faded
    assert(face.tess_games.game != TESS_GAME_SKY); face.grav_y = 0;
}
static void the_first_sky_is_answered_by_a_finger_and_resolves_into_the_next(void) {
    answer();
    until_phase(SKY_PLAY, 4);
    assert(sky()->w.count_stars == 1 && sky()->w.count_tools == 0 && face.tess_games.sky_elements == 1);
    const sky_star_t *star = &sky()->w.star[0];
    for (int i = 0; i < 40 * 30 && sky()->phase == SKY_PLAY; i++) { face.rub_in = (rub_input_t){.down = true, .x = star->x, .y = star->y}; frame(); }
    face.rub_in = (rub_input_t){0};
    assert(sky()->phase == SKY_RESOLVE && face.tess_games.sky_solved == 1 && face.tess_games.sky_elements == 2);
    until_phase(SKY_PLAY, 8);   // the next sky has grown, with one thing more
    assert(sky()->w.kind == SKY_POUR && sky()->solved_here == 1 && sky()->compose == 0);
}
static void every_exit_ends_a_sky_at_once(void) {
    for (int exit = 0; exit < 6; exit++) {
        answer(); until_phase(SKY_PLAY, 4); run(1);
        switch (exit) {
        case 0: face_card(&face, "An incoming agent reply"); break;
        case 1: face.menu.open = true; break;
        case 2: face.dark = true; break;
        case 3: face_event(&face, FEV_NOTIFY, 0, 0); break;
        case 4: tess_games_set_available(&face, false); break;
        default: break;
        }
        if (exit == 5) {   // a short BOOT: the stars fade and she folds back
            assert(tess_games_end_sky(&face));
            run(.5f); assert(face.tess_games.game == TESS_GAME_SKY && sky()->phase == SKY_FADE);
            run(2.5f);
        } else frame();
        assert(!face.tess_games.game && !sky());
        run(1); assert(face.tess_games.sky_lit == 0);   // no voice is left sounding
        face.dark = false; face.menu.open = false;
    }
    fresh(TESS_PROGRESS_MASK); assert(!tess_games_end_sky(&face));   // BOOT with no sky is the ordinary BOOT
}
static void what_was_learnt_is_kept_and_restored(void) {
    fresh(TESS_PROGRESS_MASK);
    face.tess_games.sky_elements = 5; face.tess_games.sky_skill = 173; face.tess_games.sky_root = 3; face.tess_games.sky_solved = 4021;
    bool settled; uint32_t kept = tess_games_sky_progress(&face, &settled);
    assert(settled && kept);
    fresh(TESS_PROGRESS_MASK);
    assert(face.tess_games.sky_elements == 0);
    tess_games_restore_sky(&face, kept);
    assert(face.tess_games.sky_elements == 5 && face.tess_games.sky_skill == 173 && face.tess_games.sky_root == 3 && face.tess_games.sky_solved == 4021);
    tess_games_restore_sky(&face, 15u | 7u << 12);   // nonsense is not taken
    assert(face.tess_games.sky_elements == 5 && face.tess_games.sky_root == 3);
    tess_games_restore(&face, TESS_PROGRESS_MASK);   // a reset forgets it
    assert(face.tess_games.sky_elements == 0 && face.tess_games.sky_solved == 0);
}
#include "tess_games_integration_sky.h"
int main(void) {
    quiet_releases_survive_a_full_queue();
    pouring_uses_gravity_without_rest_pose_drift();
    an_ignored_invitation_asks_nothing();
    skies_summon_play_and_sing();
    the_first_sky_is_answered_by_a_finger_and_resolves_into_the_next();
    she_asks_only_when_lying_down_and_a_rock_summons_her();
    raising_the_cube_answers_an_invitation_like_a_touch();
    every_exit_ends_a_sky_at_once();
    what_was_learnt_is_kept_and_restored();
    a_rolled_corner_pulls_towards_the_screen_finger();
    visible_corners_are_the_touch_targets();
    background_events_preserve_a_round();
    an_upright_device_is_her_level();
    tilting_rolls_her_against_the_rim_and_home();
    every_burst_is_its_own_organic_spread();
    tilting_the_burst_pours_her_dots_home();
    her_temper_changes_how_she_answers();
    a_knock_rattles_her();
    her_movements_sound_like_themselves();
    a_tap_plucks_a_wave_through_her_edges();
    plucked_corners_ring_one_body_not_a_scale();
    two_quick_plucks_on_different_sides_echo();
    swipes_wind_her_up_until_she_blooms();
    a_finger_beside_her_stretches_and_twangs();
    a_far_pull_slings_her_into_the_rim();
    a_tap_catches_her_in_flight();
    the_hardest_throw_shatters_her_and_a_sweep_gathers_her();
    rapid_taps_still_burst_and_the_burst_returns_alone();
    a_drag_on_her_still_turns_her();
    alone_she_plays_with_herself();
    priority_stops_play_at_once();
    invalid_taps_never_poison_native_touch_state();
    assert(most_dots >= TESS_N && most_dots <= (unsigned)R_MAX_DOTS);
    puts("native Tess play: skies, tilt, pour, organic bursts, temper, rattle, body sounds, pluck, echo, bloom, stretch, sling, catch, shatter, gather, self-play and priority ok");
}
