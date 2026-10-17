#pragma once
#include <math.h>
#include <stdint.h>
#include "tess.h"
#include "face_math.h"
#include "tess_points.h"

#define PI 3.14159265f
#define TESS_PX_PER_UNIT 77.f  // panel pixels to one unit of the cloud, in the plane of its middle (90 px * 4 / 4.7)
#define TESS_TREMBLE_FROM .28f  // push below which the dots do not tremble (and may be held to their rigid places)
#define TESS_LOOK_YAW .25f    // rad the camera turns towards what the cloud attends to (tess_look, -1..1)
#define TESS_LOOK_PITCH .16f
#define TESS_CONTENT_S 5.f     // s of the contented sway after being petted

// Reactions that move the body as a whole (rigid: a shove of the carried offset, a turn, a roll).
enum { KICK_HOP, KICK_WIGGLE, KICK_TWIRL, KICK_FLINCH, KICK_NUZZLE, KICK_NOD, KICK_SHIVER, KICK_DIP, KICK_COUNT };
// What can follow later (tess_after): any of the kicks, or one of these.
enum { AFTER_SWING = KICK_COUNT, AFTER_UNTURN, AFTER_PEEK, AFTER_LOOK_AWAY, AFTER_BOO, AFTER_CONTENT, AFTER_JOY_CUE, AFTER_NONE = 0xFF };

#define TESS_HANG_S .82f   // s into a burst where the dots hang out on the glass (tess_scatter.c), and Gather holds them
#define TESS_BURST_S 2.6f  // s a hard shake's burst lasts: the choreography of tess_scatter.c (2.5 s) and a moment of the dots at rest

typedef struct { int x, y, radius, key; uint16_t lit; } tess_projected_t;
void tess_project_dots(face_t *face, tess_projected_t dots[TESS_N]);
// Play moves her own structure (tess_play_shape.c): once a frame with her vertices, then each dot by its form slot.
bool tess_play_frame(face_t *face, const float points[TESS_N][3], float scale, float body_dx, float body_dy, float body_cos, float body_sin);
int tess_play_vertex_slot(int vertex);
int tess_play_slot_vertex(int slot);
void tess_play_point(int slot, float *x, float *y, float *z);

float tess_sin(float radians);
void tess_vertices4d(const face_t *face, float vertices[16][4]);  // the tesseract after its 4D turns, before any projection
void tess_growth_vertices(const face_t *face, float vertices[16][4]);  // the game’s point/square/cube lift; adult path is exact
typedef struct { float c[3], s[3]; bool nod; } tess_turn4d_t;      // the x-w, z-w and y-w turns as cosines and sines
void tess_turn4d_prepare(const face_t *face, tess_turn4d_t *turn);
void tess_turn4d_prepare_scaled(const face_t *face, float fourth, tess_turn4d_t *turn);  // grows 4D turns with the fourth axis
void tess_turn4d_apply(const tess_turn4d_t *turn, float vector[4]);
void tess_camera_turns(const face_t *face, float turn[6]);
void tess_camera_apply(const float turn[6], float vector[3]);
void tess_mood_reset(face_t *face);
// Mood (tess_feel.c): events in, a style and a word out
void tess_feel_reset(face_t *face);
void tess_feel_update(face_t *face, float dt);                       // once a frame, first: the machine, the style, the entry cue
void tess_feel_event(face_t *face, mood_event_t event, float strength, float side);
void tess_feel_emotion(face_t *face, emotion_t emotion);            // an emotion was set on it (tags, welcome, the heart)
void tess_feel_touch_event(face_t *face, face_event_t event);
void tess_feel_mode(face_t *face, face_mode_t previous, face_mode_t mode);
// How tired of being left alone it is, 0..1: it starts after 20 s and takes `span` seconds to be as tired as it gets.
static inline float tess_quiet(float seconds, float span) { return smooth01((seconds - 20.f) / span); }
void tess_mood_kick(face_t *face, int kind, float strength, float touch_x, float touch_y);
void tess_rub_targets(face_t *face, float to[TP_COUNT], float *spin);  // the pose the rubbing asks for (tess_rub.c)
void tess_rub_step(face_t *face, float dt);                             // stage changes, the next move, warmth, the heart (tess_rub.c)
void tess_mood_event(face_t *face, face_event_t event, float x);
void tess_kick(face_t *face, int kind, float strength);    // a reaction of the whole body (tess_motion.c)
void tess_react(face_t *face, tess_reaction_t reaction, float seconds);  // a shape or colour of a mood; 0 seconds: its own length
// Play (tess_touch.c, tess_play.c)
void tess_cue(face_t *face, tess_cue_t cue, float strength, float position);  // asks the speaker for a sound (face_take_cue)
void tess_tone(face_t *face, tess_tone_kind_t kind, float hz, float gain, float shade);  // a sound of her body (face_take_tone)
void tess_tone_later(face_t *face, tess_tone_kind_t kind, float hz, float gain, float shade, float delay_s);
void tess_tone_slot(face_t *face, tess_tone_kind_t kind, unsigned slot, float hz, float gain, float shade, float delay_s);  // a held voice of a game (tess_sky.c)
float tess_tone_hz(const face_t *face, float height);  // her resonance: continuous, mood sets its rest, size its register
void tess_after(face_t *face, float seconds, int what, float strength);       // a reaction that follows on later (KICK_* or AFTER_*)
void tess_play_reset(face_t *face);
void tess_play_glad(face_t *face);      // it is delighted: a joyful bounce and twirl
bool tess_play_touched(face_t *face);   // a finger landed or tapped: the games notice; true when that made it glad
void tess_play_event(face_t *face, face_event_t event);  // whatever else happened to it: it is no longer alone
void tess_play_update(face_t *face, float dt);           // the queue, the invitations and tricks, the contented sway
void tess_play_targets(face_t *face, float to[TP_COUNT], float *spin);  // the pose a held look or a contented sway asks for
void tess_touch_update(face_t *face, float dt);          // a finger on it: it looks, follows, is turned, flung
float tess_tremble(const face_t *face);                  // the random push each dot gets this frame (units/s), 0 at rest
// a press's wave is still running (the dots are then left to their springs)
bool tess_ripple_active(const face_t *face);
typedef struct { float x, y, ring, near2, far2, fade; } tess_ripple_frame_t;
unsigned tess_ripple_frame(const face_t *face, tess_ripple_frame_t out[TESS_RIPPLES]);
void tess_ripple_point(const tess_ripple_frame_t *ripples, unsigned count, float *x, float *y, float *z);
static inline void tess_let_go(tess_mood_t *mood, float seconds) { if (seconds > mood->free_t) mood->free_t = seconds; }  // the attention lets go of the pose for a while
void tess_mood_update(face_t *face, float dt);
// Attention (tess_gaze.c)
void tess_gaze_step(face_t *face, float dt, float pose_to[TP_COUNT], float *spin);
void tess_gaze_event(face_t *face, face_event_t event);
void tess_gaze_vibration(face_t *face, float jolt);
void tess_gaze_peek(face_t *face);                   // a quarter turn through the 4th dimension takes its face out of sight (tess_gaze.c)
void tess_gaze_notice(face_t *face, float seconds);  // it turns to face the viewer, for at least so long
float tess_delay(int point);
// The burst (tess_scatter.c)
void tess_scatter_begin(face_t *face);                    // a hard shake: starts it, unless one is under way
void tess_scatter_step(face_t *face, float dt);           // the clock, the weight of the whole cloud in it, the snap's cue
float tess_scatter_weight(float seconds);                 // how far the cloud is spread, 0 formed .. 1, seconds into the burst
void tess_scatter_spot(int slot, float spot[3]);          // where a slot of the burst lands
void tess_scatter_spot_px(int slot, float *x, float *y);   // and where that is on the panel
void tess_scatter_frame(const face_t *face, float seconds);                // what the whole cloud does seconds into the burst: once a frame, before the dots
void tess_scatter_apply(float point[3], int dot, int slot);  // the dot's place in that moment
void tess_scatter_finish(face_t *face);                   // every dot is home already: the burst ends now
void tess_scatter_hang_at(int slot, float x, float y);    // a game that holds the burst places a slot (panel px)
void tess_scatter_reset(void);                            // a fresh Tess: the next spreads are laid out from the start
// Gather's hold on the burst (tess_gather.c): whether it hangs, and whether a slot was swept home (and how long ago).
bool tess_games_holds_burst(const face_t *face);
float tess_games_gathered(const face_t *face, int slot, float *since);
float tess_games_gathered_share(const face_t *face);  // 0..1 of the dots home in a Gather round
void tess_gather_pour(face_t *face, int slot);         // a dot the tilt slid through her core is home
enum { TS_FORM = 0, TS_GLOBE, TS_HEART, TS_DRIFT, TS_SCATTER };  // the forms the dots are matched to (tess_rematch)
void tess_point_targets(face_t *face, float targets[TESS_N][3], int parity);
void tess_springs(face_t *face, const float targets[TESS_N][3], float dt);
int tess_wanted_shape(const face_t *face);
void tess_rematch(face_t *face, int shape);
void tess_match_prepare(void);  // the one-off work of tess_rematch, done before the first morph
void tess_fall_turn(face_t *face, bool falling);
void tess_fall_step(face_t *face, float dt);
#define TESS_GLINT_PEAK 255  // a twinkle with this peak is a glint
float tess_twinkle_level(const face_t *face, int slot);  // offline shimmer: how lit each of the twinkling points is, 0..1
void tess_view_matrix(const face_t *face, float matrix[3][3]);
void tess_transform_point(const float matrix[3][3], const float source[3], float target[3], bool transpose);

void tess_touch_wave(face_t *face, float x, float y);
void tess_touch_resume(face_t *face, float x, float y, float age);
void tess_touch_waves_step(face_t *face, float dt);

// Skies (tess_sky.c, tess_sky_draw.c)
bool tess_sky_invite(face_t *face);                      // she asks with a gold light, now and then
float sky_summon_shown(const face_t *face); // invitation visibility, 0..1
bool tess_sky_calling(const face_t *face, float *x, float *y);   // that star is out, and where (panel px)
bool tess_sky_tap(face_t *face, float x, float y);       // a tap on the invitation starts a sky
bool tess_sky_start(face_t *face);
void tess_sky_release(tess_games_t *games);              // the sky's memory goes back
void tess_sky_stop(face_t *face);                        // its voices let go
void tess_sky_tick(face_t *face, float dt);              // every frame: stray voices, the invitation's clock
bool tess_sky_holds_burst(const face_t *face);
void tess_sky_update(face_t *face, float dt);
void tess_sky_draw(const face_t *face, scene_t *scene);
int tess_sky_dot_colour(const face_t *face, int point);
