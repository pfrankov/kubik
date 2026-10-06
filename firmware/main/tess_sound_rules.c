#include "tess_sound_rules.h"

// The voices. Every one is a pad-like ensemble: a carrier that a modulator at a whole-number ratio bends a little (the
// index is low, under 1 rad, and melts away over ring_ms, so there is no sharp partial), a copy of it detuned by a
// fraction of a percent that beats slowly against it, and a soft layer (an octave down, a fifth or an octave up; a third
// harmonic for the deep ones, which the small speaker plays though it plays nothing under 200 Hz). Nothing is noise and
// nothing wobbles. Attacks are soft swells: 28..45 ms for the quick reactions (audible at once, never a strike), 70..170
// ms for the pads. Ratios and indices in Q8, shares in Q15, detunes in 1/1024 of the pitch.
const timbre_t k_timbres[TIMBRE_COUNT] = {
    //           ratio index ring attack layer layer-share sustain release detune share
    [SOFT]    = {256, 150, 220, 40, 512, 4500, 0, 0, 5, 8000},
    [NUDGE]   = {256, 110, 120, 28, 384, 4500, 0, 0, 7, 7000},
    [GLOW]    = {512, 110, 400, 90, 128, 9000, 0, 0, 6, 9000},
    [SWEEP]   = {256, 0, 0, 45, 512, 4000, 26000, 120, 7, 9000},
    [PAD]     = {256, 60, 500, 170, 128, 9000, 0, 0, 5, 9000},
    [CLUSTER] = {512, 90, 250, 70, 384, 5000, 0, 0, 16, 13000},
    [HUM]     = {256, 60, 500, 130, 768, 4500, 0, 0, 8, 10000},
};

// RULE(timbre, contour, mask, notes min, max, low, span, spacing min, max, decay, gain, extras...): see rule_t.
// UI gains use A-weighted 100-ms calibration (tools/test-tess.py).
// Character cues retain their expressive RMS classes: whisper, character, alert.
#define RULE(timbre_, contour_, mask_, n_min, n_max, low_, span_, sp_min, sp_max, decay_, gain_, ...)              \
    {.timbre = timbre_, .contour = contour_, .mask = mask_, .notes_min = n_min, .notes_max = n_max, .low = low_, \
     .span = span_, .spacing_min = sp_min, .spacing_max = sp_max, .decay_ms = decay_, .gain = gain_, __VA_ARGS__}

// ---------------------------------------------------------------- what the app asks for
// The notes are few, long and unhurried: a run is two to four pads a quarter of a second or more apart, each one
// swelling in and dying away slowly; only the quick reactions (tap, touch, snap, fling) are short, and even they swell.
#define LOVE_RULE(gap) RULE(PAD, RISE, TRIAD, 3, 4, A3, 4, 300, 360, 1500, 2100, .send = 5, .rank = 3, .hold_ms = 1400, .gap_ms = gap, .flags = UTTERANCE, .harmony = H_WARM)
#define JOY_RULE(gap) RULE(GLOW, RISE, ANY, 3, 4, D4, 4, 150, 200, 900, 1763, .send = 4, .rank = 2, .hold_ms = 900, .gap_ms = gap, .flags = UTTERANCE, .harmony = H_BRIGHT)
#define SAD_RULE(gap) RULE(PAD, FALL, MINOR, 3, 3, B3, 3, 330, 420, 1400, 1565, .send = 4, .rank = 2, .hold_ms = 1100, .gap_ms = gap, .flags = UTTERANCE, .harmony = H_SOMBER)
const rule_t k_event_rules[SFX_COUNT] = {
    [SFX_BOOT] = RULE(PAD, RISE, FIFTHS, 3, 3, A3, 3, 260, 320, 1500, 767, .send = 5, .rank = 3, .hold_ms = 1300, .harmony = H_BRIGHT),
    [SFX_LISTEN_START] = RULE(SWEEP, RISE, FIFTHS, 2, 2, D4, 1, 22, 34, 90, 633, .bend = 3, .rank = 3, .flags = CAPTURE),
    [SFX_LATCH_START] = RULE(SWEEP, RISE, FIFTHS, 2, 2, D4, 1, 60, 80, 80, 737, .bend = 2, .rank = 3, .flags = CAPTURE),
    [SFX_LISTEN_STOP] = RULE(SWEEP, FALL, CHIME, 2, 2, E4, 1, 22, 34, 90, 1122, .bend = -3, .rank = 3, .flags = CAPTURE),
    [SFX_THINK] = RULE(SWEEP, REPEAT, ANY, 1, 1, E4, 0, 0, 0, 450, 752, .vary = 1, .bend = 3, .send = 2, .hold_ms = 800),
    [SFX_NOTIFY] = RULE(GLOW, RISE, CHIME, 2, 2, E4, 1, 260, 320, 1400, 793, .send = 5, .rank = 3, .hold_ms = 1100, .flags = HALO),
    [SFX_ERROR] = RULE(HUM, FALL, LOW_FIFTH, 2, 2, B3, 1, 280, 340, 1200, 932, .bend = -1, .send = 3, .rank = 3, .hold_ms = 900),
    [SFX_NOT_HEARD] = RULE(SOFT, REPEAT, ANY, 2, 2, E4, 0, 280, 340, 900, 1088, .bend = -1, .send = 3, .rank = 2, .hold_ms = 700, .flags = UTTERANCE),
    [SFX_CONNECT] = RULE(GLOW, RISE, FIFTHS, 3, 3, D4, 2, 230, 290, 1400, 884, .send = 5, .rank = 3, .hold_ms = 1100, .flags = UTTERANCE, .harmony = H_BRIGHT),
    [SFX_DISCONNECT] = RULE(GLOW, FALL, FIFTHS, 3, 3, D4, 2, 240, 300, 1400, 879, .bend = -1, .send = 5, .rank = 3, .hold_ms = 1100, .flags = UTTERANCE, .harmony = H_SOMBER),
    [SFX_WAKE] = RULE(SOFT, RISE, LOW_FIFTH, 2, 2, B3, 1, 200, 260, 1000, 1015, .send = 4, .rank = 2, .hold_ms = 800, .flags = HALO),
    [SFX_TAP] = RULE(SOFT, REPEAT, ANY, 1, 1, D4, 0, 0, 0, 600, 1379, .pan = 1, .vary = 2, .send = 3, .rank = 1, .hold_ms = 900, .gap_ms = 80),
    [SFX_GIGGLE] = RULE(SOFT, ZIGZAG, ANY, 2, 3, Fs4, 3, 140, 180, 500, 477, .vary = 2, .send = 3, .rank = 1, .hold_ms = 900, .gap_ms = 150, .flags = UTTERANCE),
    [SFX_PET] = RULE(PAD, RISE, TRIAD, 2, 3, D4, 2, 160, 220, 1300, 921, .pan = 1, .vary = 1, .send = 4, .rank = 2, .hold_ms = 900, .gap_ms = 200, .flags = HALO),
    [SFX_DIZZY] = RULE(CLUSTER, ZIGZAG, ANY, 2, 2, Fs4, 2, 200, 260, 1000, 816, .vary = 1, .send = 3, .rank = 2, .hold_ms = 800, .gap_ms = 300),
    [SFX_SURPRISE] = RULE(GLOW, REPEAT, ANY, 1, 1, D4, 0, 0, 0, 900, 764, .pan = 1, .vary = 1, .bend = 5, .send = 3, .rank = 2, .hold_ms = 800, .gap_ms = 300, .flags = HALO),
    [SFX_VOLUME] = RULE(SOFT, REPEAT, ANY, 1, 1, D4, 0, 0, 0, 450, 724, .climb = 8, .send = 2, .hold_ms = 700),
    [SFX_HELLO] = RULE(PAD, RISE, TRIAD, 3, 3, D4, 2, 240, 300, 1500, 1207, .send = 4, .rank = 3, .hold_ms = 1200, .flags = UTTERANCE, .harmony = H_WARM),
    [SFX_SCREEN_ON] = RULE(SOFT, RISE, TRIAD, 3, 3, A3, 2, 160, 200, 600, 1267, .send = 3, .rank = 1, .hold_ms = 900),
    [SFX_SCREEN_OFF] = RULE(SOFT, FALL, TRIAD, 3, 3, A3, 2, 170, 210, 700, 1239, .bend = -1, .send = 3, .rank = 1, .hold_ms = 900),
    [SFX_SETUP] = RULE(SOFT, ARCH, OPEN, 3, 3, E4, 2, 260, 320, 900, 515, .send = 4, .rank = 3, .hold_ms = 1000, .flags = HALO),
    [SFX_SETUP_PHONE] = RULE(SOFT, RISE, TRIAD, 2, 2, D4, 1, 180, 220, 700, 1022, .send = 3, .rank = 2, .hold_ms = 600),
    [SFX_SETUP_WAIT] = RULE(HUM, REPEAT, ANY, 1, 1, A3, 0, 0, 0, 1500, 2618, .send = 5, .hold_ms = 1000),
    [SFX_SETUP_OK] = RULE(GLOW, RISE, TRIAD, 3, 4, D4, 3, 200, 250, 1200, 816, .send = 5, .rank = 3, .hold_ms = 1200, .harmony = H_BRIGHT),
    [SFX_SETUP_FAIL] = RULE(HUM, FALL, OPEN, 2, 2, A3, 1, 280, 340, 1000, 1080, .bend = -1, .send = 3, .rank = 3, .hold_ms = 800),
    [SFX_MENU_OPEN] = RULE(SWEEP, RISE, FIFTHS, 2, 2, D4, 2, 60, 90, 110, 434, .bend = 3, .send = 2, .rank = 1, .hold_ms = 700),
    [SFX_MENU_CLOSE] = RULE(SWEEP, FALL, FIFTHS, 2, 2, D4, 2, 60, 90, 110, 1118, .bend = -3, .send = 2, .rank = 1, .hold_ms = 700),
    [SFX_DETENT] = RULE(SOFT, REPEAT, ANY, 1, 1, A3, 0, 0, 0, 380, 896, .climb = 10, .send = 2, .hold_ms = 700),
    [SFX_GLINT] = RULE(GLOW, REPEAT, ANY, 1, 1, D4, 0, 0, 0, 500, 1493, .climb = 5, .send = 2, .hold_ms = 800, .flags = HALO),
    [SFX_ARM] = RULE(SOFT, REPEAT, OPEN, 2, 2, E4, 0, 240, 290, 450, 1057, .send = 2, .rank = 1, .hold_ms = 800),
    [SFX_DISARM] = RULE(SOFT, REPEAT, OPEN, 1, 1, E4, 0, 0, 0, 500, 1641, .bend = -1, .send = 2, .hold_ms = 800),
    [SFX_DENY] = RULE(SOFT, REPEAT, ANY, 1, 1, E4, 0, 0, 0, 450, 1680, .bend = -1, .rank = 1),
    [SFX_PAGE] = RULE(SOFT, REPEAT, LOW_FIFTH, 1, 1, Fs4, 0, 0, 0, 450, 895, .bend = 1, .send = 2, .hold_ms = 700, .gap_ms = 100),
    [SFX_DISMISS] = RULE(SOFT, REPEAT, ANY, 1, 1, A4, 0, 0, 0, 350, 1397, .bend = -2, .send = 2, .rank = 1, .hold_ms = 700),
    [SFX_POWER_OFF] = RULE(PAD, FALL, FIFTHS, 3, 3, D4, 3, 300, 360, 1500, 491, .send = 4, .rank = 3, .hold_ms = 1200, .harmony = H_SOMBER),
};

// ---------------------------------------------------------------- what Tess's behaviour asks for
const rule_t k_cue_rules[TC_COUNT] = {
    [TC_TOUCH] = RULE(NUDGE, REPEAT, ANY, 1, 1, D4, 0, 0, 0, 130, 504, .pan = 2, .vary = 1, .gap_ms = 140),
    [TC_FLING] = RULE(SOFT, RISE, ANY, 1, 2, D4, 2, 100, 130, 700, 1742, .pan = 3, .bend = 2, .send = 3, .rank = 1, .hold_ms = 600, .gap_ms = 400, .flags = BY_STRENGTH | SCALED | BRIGHTEN),
    [TC_SWING] = RULE(SWEEP, FALL, ANY, 2, 2, A4, 1, 120, 160, 700, 1836, .pan = 3, .bend = -3, .send = 3, .rank = 1, .hold_ms = 600, .gap_ms = 400, .flags = SCALED),
    [TC_EXCITE] = RULE(SOFT, REPEAT, ANY, 1, 1, D4, 0, 0, 0, 500, 1824, .climb = 7, .send = 3, .rank = 1, .hold_ms = 900, .gap_ms = 110, .flags = SCALED),
    [TC_RUB] = RULE(SOFT, RISE, ANY, 1, 4, D4, 4, 100, 130, 600, 2041, .climb = 4, .vary = 1, .send = 3, .rank = 2, .hold_ms = 700, .gap_ms = 350, .flags = BY_STRENGTH | SCALED | BRIGHTEN | HALO),
    [TC_GLANCE] = RULE(SWEEP, RISE, ANY, 2, 2, E4, 1, 60, 90, 160, 614, .pan = 2, .vary = 1, .gap_ms = 1500),
    [TC_PEEK] = RULE(SWEEP, RISE, ANY, 2, 2, D4, 2, 160, 210, 600, 1267, .bend = 1, .send = 4, .rank = 1, .hold_ms = 800, .gap_ms = 1200, .flags = HALO),
    [TC_INVITE] = RULE(SOFT, RISE, FIFTHS, 2, 2, A3, 1, 240, 300, 1000, 1349, .send = 3, .rank = 1, .hold_ms = 800, .gap_ms = 9000, .flags = UTTERANCE),
    [TC_DODGE] = RULE(NUDGE, REPEAT, ANY, 1, 1, A4, 0, 0, 0, 300, 1729, .bend = -2, .rank = 1, .gap_ms = 600),
    [TC_STARTLE] = RULE(CLUSTER, ZIGZAG, ANY, 2, 2, D4, 1, 70, 100, 600, 2486, .bend = 3, .send = 2, .rank = 2, .hold_ms = 600, .gap_ms = 800, .flags = HALO),
    [TC_CONTENT] = RULE(PAD, FALL, TRIAD, 3, 3, D4, 2, 320, 380, 1600, 1348, .send = 5, .rank = 1, .hold_ms = 1200, .gap_ms = 2500, .flags = UTTERANCE, .harmony = H_WARM),
    [TC_MISCHIEF] = RULE(SOFT, RISE, LOW_FIFTH, 2, 2, Fs4, 1, 240, 300, 700, 1540, .bend = -1, .send = 3, .rank = 1, .hold_ms = 700, .gap_ms = 4000, .flags = UTTERANCE),
    [TC_LOVE] = LOVE_RULE(10000),
    [TC_JOY] = JOY_RULE(6000),
    [TC_SAD] = SAD_RULE(8000),
    [TC_TINKLE] = RULE(HUM, REPEAT, ANY, 1, 1, D4, 0, 0, 0, 600, 530, .pan = 2, .vary = 2, .send = 5, .hold_ms = 600, .gap_ms = 7000),
    [TC_IMPACT] = RULE(SOFT, REPEAT, ANY, 1, 1, Fs4, 0, 0, 0, 10, 65, .send = 0, .gap_ms = 0),
    [TC_SNAP] = RULE(NUDGE, RISE, FIFTHS, 2, 2, D4, 1, 70, 90, 450, 1443, .bend = -2, .send = 3, .rank = 1, .hold_ms = 900, .gap_ms = 1500),
    // The moods' words (gains to be measured with tools/tess-sound-report.py): once a mood, not every touch.
    [TC_CURIOUS] = RULE(SWEEP, RISE, OPEN, 2, 2, A3, 2, 160, 200, 500, 1400, .pan = 2, .vary = 1, .bend = 2, .send = 3, .rank = 1, .hold_ms = 800, .gap_ms = 12000, .flags = UTTERANCE | SCALED),
    [TC_PLAYFUL] = RULE(SOFT, ARCH, TRIAD, 3, 4, D4, 3, 130, 170, 500, 1840, .pan = 2, .vary = 2, .send = 3, .rank = 1, .hold_ms = 700, .gap_ms = 6000, .flags = UTTERANCE | BY_STRENGTH | SCALED),
    [TC_SCARED] = RULE(CLUSTER, REPEAT, ANY, 2, 3, Fs4, 0, 110, 150, 700, 2750, .pan = 2, .vary = 1, .bend = 1, .send = 2, .rank = 1, .hold_ms = 900, .gap_ms = 4000, .flags = UTTERANCE | BY_STRENGTH | SCALED, .harmony = H_TENSE),
    [TC_GRUMPY] = RULE(CLUSTER, REPEAT, LOW_FIFTH, 2, 2, B3, 0, 200, 260, 900, 2850, .vary = 1, .bend = -1, .send = 1, .rank = 1, .hold_ms = 800, .gap_ms = 8000, .flags = UTTERANCE | SCALED, .harmony = H_LOW),
    [TC_LONELY] = RULE(HUM, FALL, OPEN, 2, 3, A4, 2, 360, 440, 950, 1140, .vary = 1, .send = 6, .rank = 1, .hold_ms = 1300, .gap_ms = 12000, .flags = UTTERANCE, .harmony = H_AIRY),
    [TC_SETTLE] = RULE(PAD, FALL, FIFTHS, 2, 2, A3, 1, 300, 380, 900, 829, .vary = 1, .send = 6, .rank = 0, .hold_ms = 800, .gap_ms = 15000, .flags = SCALED),
};
