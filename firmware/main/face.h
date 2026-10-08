// Kubik's face: an expressive two-eye character drawn from SDF primitives.
// Portable C (also compiled by the host simulator in firmware/sim).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "render.h"
#include "character.h"
#include "rub.h"
#include "agent_menu.h"
#include "tess_cue.h"
#include "tess_feel.h"
#include "tess_games.h"
#include "event_journal.h"
#include "device_status.h"

typedef enum {
    EMO_NEUTRAL = 0,
    EMO_HAPPY,
    EMO_JOY,
    EMO_LOVE,
    EMO_SAD,
    EMO_ANGRY,
    EMO_SURPRISED,
    EMO_CONFUSED,
    EMO_SLEEPY,
    EMO_THINKING,
    EMO_WINK,
    EMO_SHY,
    EMO_PROUD,
    EMO_DIZZY,
    EMO_COUNT
} emotion_t;

typedef enum {
    MODE_BOOT = 0,
    MODE_IDLE,       // ready, living idle behaviour
    MODE_LISTENING,  // capture gate open (key held)
    MODE_THINKING,   // transcribing / agent working
    MODE_SPEAKING,   // speech is audible
    MODE_SLEEP,      // screen resting
    MODE_SETUP,      // Wi-Fi setup portal active
    MODE_OFFLINE,    // Wi-Fi connected, but no server welcome handshake
} face_mode_t;

typedef enum {
    FEV_TAP = 0,   // x,y = touch point
    FEV_PET,       // long touch / stroke
    FEV_SHAKE,
    FEV_PICKUP,
    FEV_NOTIFY,    // proactive message arriving
    FEV_NOT_HEARD, // empty transcript
    FEV_FAIL,      // agent/voice failure
    FEV_WAKE,
    FEV_VOLUME,    // x = level 0..1
    FEV_TALK_START,
} face_event_t;

// System bubble: a pill at the top with an icon and, when the icon alone is
// not enough, a few words (the words come from strings.c).
typedef enum {
    BUB_NONE = 0,
    BUB_ERROR,      // "!"
    BUB_NOT_HEARD,  // "?"
    BUB_BUSY,       // hourglass
    BUB_NO_WIFI,    // Wi-Fi with a slash
    BUB_NO_SERVER,  // broken link: OpenClaw unreachable
    BUB_TALK,       // microphone: how to talk
    BUB_KEY,        // pairing / not allowed
    BUB_PLUGIN,     // plug: Kubik is not installed in OpenClaw
    // Status (what is happening now), not problems:
    BUB_REC,        // microphone on red: recording
    BUB_THINK,      // running dots: OpenClaw is working on it
    BUB_OK,         // check mark: connected, paired, saved
    BUB_VOLUME,     // speaker
    BUB_STOP,       // square: stopped
    // What OpenClaw is doing (its status reactions: 🛠️ 💻 🌐 🛫 🏗️ 🗜️; 🧠 = BUB_THINK, ⏳ = BUB_BUSY):
    BUB_TOOL,       // a wrench
    BUB_CODE,       // </>
    BUB_WEB,        // a globe
    BUB_DEPLOY,     // an arrow taking off
    BUB_BUILD,      // stacked bricks
    BUB_COMPACT,    // two arrows pressing together
    BUB_POWER,      // the power symbol: turning off
    BUB_COUNT
} bubble_icon_t;

#define FACE_CARD_LINES 40
#define FACE_CARD_PAGE 6  // lines per page
#define FACE_NP 28  // number of animated float parameters

typedef struct {
    float v[FACE_NP];
} face_params_t;

typedef struct {
    float x, y, vx, vy, life, max_life, size, spin;
    uint8_t kind;  // 0 heart, 1 sparkle, 2 zed
} particle_t;

#define FACE_MAX_PARTICLES 10

// Body animations (sprite pack names, see tools/sprites/clips.json).
typedef enum {
    BA_IDLE = 0, BA_WAVE, BA_LISTEN, BA_THINK, BA_TALK, BA_JOY, BA_LOVE, BA_SAD, BA_SURPRISED, BA_SLEEP, BA_DIZZY,
    BA_ANGRY, BA_SHY, BA_COUNT
} body_anim_t;

typedef struct {
    int8_t id[BA_COUNT];  // sprite anim index per body_anim_t (-1 = missing)
    int cur;              // body_anim_t playing, -1 = resting in the neutral pose
    float pos;            // frame position
    int dir;              // +1 forward, -1 rewinding
    bool oneshot;         // playing through once (not held)
    bool leaving;         // heading back to the neutral pose
    int req;              // pending one-shot request, -1 = none
    float rest;           // seconds resting in the neutral pose
    float next_idle;      // rest time before the next idle fidget
    float frame;          // fractional source-frame position shown this tick
    int anim;             // sprite anim index shown this tick
    float vel;            // playback speed, frames/s (signed): eases, never jumps
    int exit_k, exit_to;  // leaving: the frame to cut at, and towards which body anim (-1 = none)
} body_t;

// Settings menu, drawn instead of the character (a long BOOT press opens it).
typedef enum { MENU_VOLUME = 0, MENU_BRIGHT, MENU_ACTIONS, MENU_AGENT, MENU_GUIDE, MENU_STATUS, MENU_UI_VOLUME, MENU_EVENTS, MENU_ROWS } menu_row_t;
typedef enum { MT_WIFI = 0, MT_POWER, MT_RESET, MT_COUNT } menu_text_t;
typedef struct {
    bool open;
    bool sound;                 // separate speech and interface controls
    bool service;               // hidden factory-reset control revealed for this menu visit
    float k;                     // 0 closed .. 1 open (animated by face_update)
    const char *txt[MT_COUNT];   // words, set by the app (menu_text_t)
    int armed;                   // an action was tapped once (its part + 1, 0 = none): the next tap confirms
    int volume, ui_volume, brightness; // 0..100
    int pressed, pressed_part;   // tile flashing after a touch (-1 = none), and its side / segment / button
    float pressed_t;
} face_menu_t;

// Tess: a cloud of TESS_N points whose resting form is a tesseract turning in 4D. Reactions and modes are
// shapes the same points flow into (staggered, on springs), so every change of state is a continuous morph.
#define TESS_N 112
#define TESS_TWINKLES 40  // the offline shimmer has some 20 points in a twinkle at any moment
typedef enum {
    TR_HEART = 0, // pet / love: a beating volumetric heart
    TR_SCATTER,   // shaken hard / dizzy: blown apart, then reassembled
    TR_RISE,      // picked up / woken: lifts
    TR_SAD,       // droops, shrinks, turns deep blue
    TR_PUZZLED,   // the whole form cocks sideways
    TR_JOY,       // bounces, warm gold highlights
    TR_SURPRISE,  // a quick burst outwards
    TR_ANGRY,     // shivers, glows coral
    TR_SHY,       // pulls in and down, pink
    TR_COUNT
} tess_reaction_t;
typedef enum { TM_LISTEN = 0, TM_THINK, TM_SPEAK, TM_SLEEP, TM_OFFLINE, TM_COUNT } tess_mode_t;
// How the cube carries itself (tess_mood.c): offsets of the view, eased on springs, on top of the endless turn.
typedef enum { TP_YAW = 0, TP_PITCH, TP_ROLL, TP_COUNT } tess_pose_t;
// What Tess does with a finger and with nobody (tess_touch.c, tess_play.c).
#define TESS_QUEUE 6
#define TESS_CUES 6
#define TESS_RIPPLES 3
typedef struct { float x, y, age; } tess_ripple_t;  // the wave of a press: where it began (units, as the points), seconds ago
typedef struct {
    float look[2], look_rate[2];   // its face turned towards the finger (extra camera yaw and pitch, rad) on a spring
    float finger[2], speed[2];     // where the finger was last frame (panel px) and how fast it moves (px/s, smoothed)
    float stroke, down_t, up_t;    // px it has travelled across the glass, seconds it has been down, seconds since it lifted
    bool down, petted;             // a finger is on it; it was petted (a long stroke or hold) this time
    float alone, invite_in, waiting;  // s since anything touched it; to the next invitation or trick; left of the wait for an answer
    float dodge_t, dodge_cool, run_t;  // s it may still be caught after a dodge; before it dodges again; since the last swipe
    uint8_t invites, flings;       // unanswered invitations; swipes in a run
    float hold[3], hold_t;         // a pose (yaw, pitch, spin) it holds for hold_t seconds: a look away, a freeze
    float content_t, glow;         // s of the contented sway left; the warmth of the palette it leaves (0..1)
    float keep;                    // 0..1 how much of the idle pose stays where it is while it plays instead of going home
    float after_t[TESS_QUEUE], after_s[TESS_QUEUE];  // reactions that follow on later: seconds to go, strength
    uint8_t after_what[TESS_QUEUE];                  // (0xFF: free slot)
    struct { uint8_t cue; float strength, position; } cue[TESS_CUES];  // what the speaker is to play (face_take_cue)
    uint8_t cue_n;
} tess_play_t;

typedef struct {
    float pose[TP_COUNT], rate[TP_COUNT];  // offsets (rad) and their speeds
    float push[TP_COUNT];                  // speed a kick still has to give the pose (it is let in over ~0.2 s)
    float spin;                            // the endless turn about the vertical, rad/s, either way
    float shift[2], shift_rate[2];         // the whole cloud carried sideways / up (units): kicks, hops; springs
    float drag_angle[2], drag_speed[2];    // touch rotation and released momentum, horizontal/vertical
    float twirl, twirl_now;                // extra turn about the vertical from a twirl, rad/s, dying away; and what is turning now
    float barrel, barrel_goal, barrel_rate;  // a barrel roll: the roll angle turned, the whole turns it is heading for, its speed
    float yw, yw_goal, yw_rate;            // the playful 4D roll: y-w angle, the quarter turn it is heading for, its speed
    float warm;                            // 0..1 palette warmth: effort of the rubbing
    float wiggle;                          // roll wobble amplitude (rad), dying away
    // Attention (tess_gaze.c): it faces the viewer in turns, wanders off, glances back, peeks through the 4th dimension.
    float attention, attack;               // 0..1 how much it faces the viewer, and how fast that rises (1/s)
    float glance[2], glance_goal[2], glance_rate[2];  // micro-saccades: quick small offsets of the camera's yaw and pitch (rad)
    float aim[2], aim_rate[2];                          // the pose (yaw, pitch) that last put its face towards the viewer
    float phase_left, saccade_in, check_in, check, peek_in;  // s: this phase, to the next saccade, to the glance back, the glance left, to the peek
    float study, lean, eager, vibe_cool, react;   // the peering left (s), leaning in and eager tracking (0..1), s before it notices the next tremor
    float w_turn[3], w_goal[3], w_rate[3]; // peeks: extra x-w, z-w and y-w turns (rad) that run to their goals, whole quarters
    uint8_t cell;                          // its face: the tesseract cell 0..7 = +x, -x, +y, -y, +z, -z, +w, -w
    bool attentive;                        // in an attentive phase (else wandering)
    float rub_hold[3];                     // rubbing: what the current move holds (yaw, pitch, spin)
    float free_t;                          // s left that its attention lets go of the pose (it spins, plays) instead of facing the viewer
    float perk, dizzy;                     // 0..1: touched or picked up (fades in 5 s), shaken (fades in 2.5 s)
    float still, view_sum;                 // seconds without motion; |view_q| axes summed, to tell when it moves
    float run, len, amp, aux, next_4d;     // the move: seconds in, its length, two random traits; s until the next 4D turn
    uint8_t move;
    bool shaken;
} tess_mood_t;

typedef struct {
    character_t character;
    float tess_phase, tess_energy;  // only advanced for Tess
    float tess_position[TESS_N][3], tess_velocity[TESS_N][3];
    float tess_target[TESS_N][3];   // spring targets; half the points are re-shaped per frame (no FPU)
    float tess_wdepth[TESS_N];       // each point's fourth-axis depth for the drawing pass
    uint8_t tess_tick;
    bool tess_points_ready;
    float tess_reaction[TR_COUNT], tess_hold[TR_COUNT];
    float tess_mode[TM_COUNT];      // eased weights of the current mode's shape
    float tess_touch_x, tess_touch_y, tess_touch_t, tess_pulse;
    tess_ripple_t tess_ripple[TESS_RIPPLES];
    float tess_audio, tess_record;
    float tess_agitation;           // shaking, 0..1: jitter and brightness
    float tess_assemble;            // 0 = collapsed to a spark (just selected) .. 1 = formed
    // Its character, all in how the points move: attention (the cloud leans), fidgets, reactions.
    float tess_look[2], tess_look_rate[2], tess_look_to[2], tess_next_look;  // where it attends, -1..1 each way, and how fast it turns
    float tess_next_fidget;                           // seconds to the next small fidget
    int tess_last_kick;                               // no same reaction twice
    int tess_taps;                                    // a quick run of taps builds up
    float tess_tap_gap;
    float tess_sleep_t;                               // seconds since it fell asleep (-1 awake): the scene
    float tess_wake_t;                                // seconds since it woke
    float tess_drift;                                 // asleep: the points drifted apart, 0 formed .. 1
    // The points are interchangeable: which tesseract point and which shape slot each one plays is re-matched
    // (nearest free place) whenever the cloud heads for another form, so a change never sends points across it.
    uint8_t tess_form_of[TESS_N], tess_shape_of[TESS_N], tess_scatter_of[TESS_N];
    int8_t tess_shape;                                // the form the points were last matched to (-1 none)
    float tess_xw, tess_zw;                           // 4D turn: only while thinking; otherwise it rests
    float tess_rigid;                                 // 0..1 how settled the cloud is as the rigid tesseract (tess_motion.c)
    tess_mood_t tess_mood;                            // idle moves, and how the cube leans into what is going on
    tess_play_t tess_play;                            // its play: what a finger does to it, and what it does alone
    tess_games_t tess_games;                          // bounded native games and restored discoveries
    mood_machine_t mood;                              // Tess's mood: what lasts of what happened to it (mood.c)
    tess_style_t style;                               // ... and what it asks of the body now (tess_feel.c)
    tess_feel_t feel;                                 // ... the events waiting for it, and the word it is about to say
    rub_t rub;                                        // petting effort (both characters)
    bool rub_love;                                    // the love on show came from rubbing (Plush: no hug clip, see face_body.c)
    rub_input_t rub_in;                               // input: the finger's stroke since the last update (consumed by it)
    bool tess_fallen;                                 // offline: loose particles lying where gravity pulls
    uint8_t tess_contact_wait[TESS_N], tess_contact_cursor;
    float tess_twinkle_age[TESS_TWINKLES], tess_twinkle_rise[TESS_TWINKLES], tess_twinkle_fade[TESS_TWINKLES];  // offline shimmer: points brighten and fade, overlapping (a slot is free once age is past rise + fade)
    float tess_twinkle_next, tess_twinkle_glint_in;  // s to the next twinkle, and to the next glint
    uint8_t tess_twinkle_point[TESS_TWINKLES], tess_twinkle_peak[TESS_TWINKLES];  // which point; how bright it gets, 0..255
    face_mode_t mode;
    emotion_t emotion;
    float emotion_left;  // seconds, <0 = sticky
    float t;             // seconds since start
    float mode_t;        // seconds in current mode
    float idle_t;        // seconds without interaction

    face_params_t cur, vel, target;

    // Live inputs (set by the app every frame).
    bool live_active, live_ready, live_mic; // explicit GPT Live connection and local microphone gate
    float mic_level;  // 0..1, capture level while listening
    float spk_level;  // 0..1, level of audio currently leaving the speaker
    float tilt_x, tilt_y;  // accelerometer, -1..1
    float view_q[4];       // Tess: device orientation from its resting pose (w, x, y, z); zero = identity
    float jolt_dvx, jolt_dvy;  // input: velocity change since the last update, g*s (screen axes)
    bool dark;             // input: the panel is off, so only clocks and timers advance
    // The face is loose inside its glass: jolts knock it about, it bounces off the edge once.
    float loose_x, loose_y, loose_vx, loose_vy;  // design px, px/s
    float squash_x, squash_y;                    // brief flattening after hitting the edge
    bool loose_arm_x, loose_arm_y;               // the next edge hit still bounces
    float slide_x, slide_y;                      // input: gravity's pull along the screen, g
    float grav_x, grav_y;                        // input: gravity along the screen, absolute, g (x right, y down)
    bool battery_low, charging, battery_present, usb_power;
    int battery_pct;

    // Internal animation state.
    float blink, blink_t, next_blink;
    int blink_phase;
    float look_tx, look_ty, next_saccade;
    float hop, hop_v;
    float next_micro, micro_left;
    int micro;
    float poke_eye_t[2];
    float talk_open, talk_prev;
    float mic_hist[8];
    float listen_lvl;  // voice level while listening, smoothed (quick up, slow down)
    float sent_t;      // seconds since a phrase was sent (listening -> thinking), -1 = none
    float volume_show, volume_level;
    float notify_t;
    float dizzy_t;
    float pickup_t;  // big pick-up welcome in progress (s), <0 = none
    float boot_t;
    particle_t parts[FACE_MAX_PARTICLES];
    float spawn_t;
    uint32_t rng;
    body_t body;
    // Face screen of the current frame in panel pixels (valid when sprites are present).
    float scr_cx, scr_cy, scr_hw, scr_hh, scr_ang, scr_vis;
    bool scr_has_quad;
    float scr_quad[8];  // where the face design rectangle lands (perspective track), panel px
    bool scr_has_glass;
    float scr_gx, scr_gy, scr_gr[24];  // the visible glass outline (sprite_screen_t), panel px
    float body_breath;  // vertical scale of the settled sleep pose
    float body_dy;  // procedural hop applied to the sprite
    float body_dx, body_vx;  // sway: the plush lags behind a jolt and wobbles back (panel px, px/s)
    // Setup card (MODE_SETUP), shown instead of the body: a title, the QR as
    // large as fits (or a "connecting" sign) and one detail line under it.
    const uint8_t *qr;
    int qr_n, qr_step;  // step 1 = join Kubik's Wi-Fi, 2 = open the setup page, 3 = connecting
    float qr_t;         // seconds since the card content changed
    char setup_text[2][72];  // title, detail (UTF-8)
    char pair_code[12];      // pairing card (MODE_SETUP) while set
    // System bubble (any mode except the setup card).
    int bub_icon;
    char bub_text[48];
    float bub_t, bub_left;  // seconds shown, seconds left (<0 = until replaced)
    // Text card: what OpenClaw wants read rather than heard, or could not say.
    // Word-wrapped once when it arrives; shown a page at a time over the face.
    char card_buf[1152];             // wrapped lines, NUL-separated
    uint16_t card_line[FACE_CARD_LINES];
    int card_n, card_page;
    uint32_t card_hash;              // of the source text, to tell a longer version of it
    int card_src_len;
    float card_t, card_page_t, card_left;  // seconds shown, on this page; left (<0 = not closing yet)
    bool card_hold;                  // speech of the reply is playing: the card stays
    event_journal_t journal;
    device_status_t status;
    face_menu_t menu;
    agent_menu_t agent;
    // Cron jobs (top-right corner): how many run now; when the next one-shot job is due (face time t, <0 = none).
    int cron_running;
    float cron_due;
    float cron_k;                    // indicator fade 0..1
    int offline_icon;                // BUB_NO_WIFI / BUB_NO_SERVER while the connection is lost (0 = online)
    int offline_icon_last;           // retained only for this face while its connection icon fades
    float offline_k;                 // Tess: its corner icon fade 0..1
} face_t;

static inline character_t face_character(const face_t *f) {
#ifdef ESP_PLATFORM
    (void)f;
    return (character_t)KUBIK_CHARACTER;
#else
    return f->character;
#endif
}

void face_init(face_t *f);  // call after sprite_init()
#ifndef ESP_PLATFORM
void face_set_character(face_t *f, character_t character);  // Native previews only.
#endif
void face_set_mode(face_t *f, face_mode_t m);
void face_set_emotion(face_t *f, emotion_t e, float seconds);
void face_event(face_t *f, face_event_t ev, float x, float y);
void face_mood_off(face_t *f, bool off);  // tests, the simulator: no moods at all, a neutral Tess as before them
void face_force_mood(face_t *f, mood_t mood, float level);  // tests, the simulator, the device's test hook: Tess is in this mood and stays (CALM: released)
void face_update(face_t *f, float dt);
bool face_take_cue(face_t *f, tess_cue_t *cue, float *strength, float *position);  // Tess: the next sound it asks for, oldest first
void face_draw(face_t *f, scene_t *s);
// Setup card content. mods may be NULL (connecting sign); title NULL or ""
// hides the card. Strings are copied.
void face_set_setup(face_t *f, const uint8_t *mods, int n, int step, const char *title, const char *detail);
// Pairing card: the code OpenClaw's owner approves; NULL or "" removes it.
void face_set_pairing(face_t *f, const char *code);
// Shows a system bubble (replacing any other). text may be NULL; seconds < 0
// keeps it until the next call; icon BUB_NONE hides it.
void face_bubble(face_t *f, bubble_icon_t icon, const char *text, float seconds);
emotion_t face_emotion_from_name(const char *name);
// Text card over the face (UTF-8, "\n" breaks lines); NULL or "" closes it. A text that
// extends the one shown (the same reply growing) keeps the current page.
void face_card(face_t *f, const char *text);
// Settings menu hit test at a panel point: the tile (menu_row_t, -1 = none) and, in *part, what was hit:
// sliders: value 0..100; actions: 0 (Wi-Fi), 1 (reset), 2 (power).
int face_menu_hit(const face_t *f, int x, int y, int *part);
// Slider value 0..100 at panel y (both sliders are vertical; a finger that drifted off the tile keeps moving it).
int face_menu_slider(int y);
// Tap on the card: the next page, or closes it after the last. false: no card is up.
bool face_card_tap(face_t *f);

// Invalid readings never become a false empty-battery warning.
void face_set_power(face_t *f, bool present, int percent, bool charging, bool usb_power);
