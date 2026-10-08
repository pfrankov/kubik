#include "tess_internal.h"
#include <string.h>
#include <stdlib.h>

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

#define COL_REC 0xFF3B30u

// Colour and glow say one thing only: how near the point is to the viewer (d 0 far .. 1 near). Never white.
// Moods and modes change the hue of that ramp, not its brightness order. 32 steps, cached until the tint changes.
#define TESS_SHADES 32
typedef tess_projected_t dot_t;  // Q3 position/size, depth and twinkle

static uint32_t ramp(uint32_t far, uint32_t mid, uint32_t near, float d) {
    return d < .5f ? rgb_mix(far, mid, d * 2) : rgb_mix(mid, near, d * 2 - 1);
}
// Far to near: violet, blue, sky blue, turquoise, light turquoise (never white). Stops by the user.
static uint32_t tess_depth_ramp(float d) {
    static const uint32_t stop[] = {0x14051D, 0x102052, 0x095A96, 0x08A6CE, 0x2BE2D2, 0x7AF4DF, 0xD5FFF4};
    static const float at[] = {0, .2f, .42f, .62f, .8f, .92f, 1};
    const int n = sizeof stop / sizeof stop[0];
    float x = clampf((d - .12f) / .8f, 0, 1);  // few points are really far: the violet starts early
    int i = 0;
    while (i < n - 2 && x > at[i + 1]) i++;
    return rgb_mix(stop[i], stop[i + 1], (x - at[i]) / (at[i + 1] - at[i]));
}

// What each mood tints the depth ramp with (far, mid, near): the same order of brightness, never white. Curious is a
// fresh green, scared an electric violet, sleepy a dim grey-lilac; the others are the colours of the matching reactions.
static const uint32_t k_mood_ramp[MOOD_COUNT][3] = {
    [MOOD_CURIOUS] = {0x04180E, 0x1FA86A, 0xC4FFB0}, [MOOD_PLAYFUL] = {0x160E04, 0x8C6418, 0xFFCB5C},
    [MOOD_LOVED] = {0x2E0B28, 0xA3316F, 0xFF8AC4},   [MOOD_SCARED] = {0x140428, 0x6B2BD9, 0xD9B8FF},
    [MOOD_GRUMPY] = {0x200605, 0x9A2A1C, 0xFF7152},  [MOOD_SAD] = {0x030826, 0x1E3E9A, 0x5A86F0},
    [MOOD_SLEEPY] = {0x0A0A10, 0x343058, 0x8683B0},
};
// The ramp the moods make of the neutral one, by their colour weights (the rest of the weight stays with the neutral one).
static uint32_t tess_mood_color(const face_t *f, uint32_t c, float d) {
    float total = 1;
    for (int mood = MOOD_CURIOUS; mood < MOOD_COUNT; mood++) total -= f->feel.mix[mood];
    total = fmaxf(0.f, total);  // (the weights are rounded: they may add up to a little over one)
    for (int mood = MOOD_CURIOUS; mood < MOOD_COUNT; mood++) {
        float weight = f->feel.mix[mood];
        if (weight <= 0) continue;
        total += weight;
        c = rgb_mix(c, ramp(k_mood_ramp[mood][0], k_mood_ramp[mood][1], k_mood_ramp[mood][2], d), weight / total);
    }
    return c;
}

// Rubbing warms the palette from cyan towards amber and gold (in 1/24 steps, so the shades are rebuilt only when it changes).
static float tess_warmth(const face_t *f) { return roundf(f->tess_mood.warm * 24.f) / 24.f; }

// How far the palette has gone to black and white: with the connection gone, at once when the points fall.
static float tess_noir(const face_t *f) { return f->tess_fallen ? 1.f : f->tess_mode[TM_OFFLINE]; }

// A little warmth follows the real playback envelope, quantized to seven tint levels.
// Mood colors and the depth/brightness order still carry their existing meaning.
static float reply_tint(const face_t *f) {
    return roundf(f->tess_mode[TM_SPEAK] * (.08f + .20f * f->tess_audio) * 24.f) / 24.f;
}

static float game_tint(const face_t *f) {
    return f->tess_games.ready ? roundf(clampf(f->tess_games.pulse, 0, 1) * 8) / 8 : 0;
}

static uint32_t tess_color(const face_t *f, float d) {
    const float *r = f->tess_reaction, *mw = f->tess_mode;
    uint32_t c = tess_mood_color(f, tess_depth_ramp(d), d);
    if (f->tess_mood.warm > .01f) c = rgb_mix(c, ramp(0x1C0E06, 0xB8641A, 0xFFC868, d), tess_warmth(f) * .9f);
    if (r[TR_SAD] > .01f) c = rgb_mix(c, ramp(0x030826, 0x1E3E9A, 0x5A86F0, d), r[TR_SAD] * .8f);
    if (r[TR_ANGRY] > .01f) c = rgb_mix(c, ramp(0x200605, 0x9A2A1C, 0xFF7152, d), r[TR_ANGRY]);
    if (r[TR_JOY] > .01f) c = rgb_mix(c, ramp(0x160E04, 0x8C6418, 0xFFCB5C, d), r[TR_JOY] * .7f);
    if (r[TR_SHY] > .01f) c = rgb_mix(c, ramp(0x200A1A, 0x93406E, 0xFF9CCB, d), r[TR_SHY] * .7f);
    if (r[TR_HEART] > .01f) c = rgb_mix(c, ramp(0x2E0B28, 0xA3316F, 0xFF8AC4, d), r[TR_HEART]);
    float reply = reply_tint(f);
    if (reply > 0) c = rgb_mix(c, ramp(0x120825, 0x2BA69A, 0xFFE2A0, d), reply);
    float play = game_tint(f);
    if (play > 0) c = rgb_mix(c, ramp(0x170D04, 0x987524, 0xFFCE78, d), play * .65f);
    // No connection: noir. Black and white, over any mood, at full contrast: dark grey specks far, pure white near.
    float noir = tess_noir(f);
    if (noir > .01f) c = rgb_mix(c, ramp(0x202020, 0xA6A6A6, 0xFFFFFF, d), noir);
    float dim = 1 - .45f * mw[TM_SLEEP];
    return dim < .99f ? rgb_scale(c, dim) : c;
}

// On integers (object units * 4096, the view * 65536): per point this was ~40 soft-float operations and a
// division. Screen 240 + 90 x p, depth d = (z + wgain w + range) gain, radius (2 + 2.8 d) p dust, p = 4 / (4.7 - z).
static void project_points(const face_t *f, const float points[TESS_N][3], const float m[3][3], float camera,
                           float wgain, float dust, float depth_range, float depth_gain, dot_t dots[TESS_N]) {
    int32_t view[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) view[i][j] = (int32_t)(m[i][j] * 65536);
    int32_t camera_q = (int32_t)(camera * 4096), reach_q = (int32_t)(((int64_t)1 << 30) * camera_q / 19251);  // (the size at z = 0 stays whatever the distance)
    int32_t wgain_q = (int32_t)(wgain * 4096), range_q = (int32_t)(depth_range * 4096);
    int32_t gain_q = (int32_t)(depth_gain * 65536), dust_q = (int32_t)(dust * 4096);
    for (int i = 0; i < TESS_N; i++) {
        int32_t source[3], rotated[3];
        for (int axis = 0; axis < 3; axis++) source[axis] = (int32_t)lrintf(points[i][axis] * 4096);
        for (int axis = 0; axis < 3; axis++)
            rotated[axis] = (int32_t)(((int64_t)view[axis][0] * source[0] + (int64_t)view[axis][1] * source[1] +
                                       (int64_t)view[axis][2] * source[2] + (1 << 15)) >> 16);
        int32_t far = camera_q - rotated[2];                                   // 4.7 - z
        int32_t p = reach_q / (far > 4915 ? far : 4915);                      // 4 / max(far, 1.2), * 65536
        int32_t w = (int32_t)(f->tess_wdepth[i] * 4096);
        int64_t depth = ((int64_t)rotated[2] + ((int64_t)wgain_q * w >> 12) + range_q) * gain_q >> 12;
        int32_t d = depth < 0 ? 0 : depth > 65536 ? 65536 : (int32_t)depth;  // 0 .. 1, * 65536
        int32_t near = p < 36045 ? 36045 : p > 98304 ? 98304 : p;             // p clamped to .55 .. 1.5
        int64_t size = (int64_t)(8192 + (11469 * (int64_t)d >> 16)) * near >> 16;  // (2 + 2.8 d) near, * 4096
        dots[i] = (dot_t){1920 + (int)(((int64_t)rotated[0] * p * 720 + (1 << 27)) >> 28),
                          2040 + (int)(((int64_t)rotated[1] * p * 720 + (1 << 27)) >> 28),
                          (int)((size * dust_q + (1 << 20)) >> 21), (int)((int64_t)d * 65535 >> 16), 0};
    }
}

// Fit the conversational cloud as a whole, including its halos. Perspective can
// make a near vertex large during a 4D inversion; zoom must never clip that vertex.
// Q16 keeps this pass cheap on the C6. The gate fades out with the mode weights.
static void fit_conversation(const face_t *f, dot_t dots[TESS_N]) {
    float talk = f->live_active ? 1 : f->tess_mode[TM_THINK] + f->tess_mode[TM_SPEAK];
    talk *= 1 - f->tess_mode[TM_OFFLINE];
    if (talk <= .001f || f->tess_fallen) return;
    int rx = 1, ry = 1;
    for (int i = 0; i < TESS_N; i++) {
        rx = imax(rx, abs(dots[i].x - 1920) + dots[i].radius * 3);
        ry = imax(ry, abs(dots[i].y - 2040) + dots[i].radius * 3);
    }
    int height = f->live_active ? 140 : 198;
    int fit = imin(65536, imin((216 * 8 * 65536) / rx, (height * 8 * 65536) / ry));
    int gate = (int)(clampf(talk * 4, 0, 1) * 65536);
    fit = 65536 - (int)(((int64_t)(65536 - fit) * gate) >> 16);
    for (int i = 0; i < TESS_N; i++) {
        dots[i].x = 1920 + (int)(((int64_t)(dots[i].x - 1920) * fit) >> 16);
        dots[i].y = 2040 + (int)(((int64_t)(dots[i].y - 2040) * fit) >> 16);
        dots[i].radius = (int)(((int64_t)dots[i].radius * fit) >> 16);
    }
}

// Fallen points (no connection): black and white in four clearly different tones, one per point for good (a
// hash of its number: no depth to go by, they all lie in the screen plane), the brightest also the biggest. A point
// in motion flashes towards white, in proportion to its speed, and settles back to its own tone where it lies.
// The tones stay low (0x30..0xA0) on purpose: a twinkle needs room to rise, and the panel's dark end is crushed, so
// the field rests in the greys the panel does show. Colours are mixed in coded (perceptual) values, where equal steps
// look equal on the panel.
static const uint32_t k_noir_tone[4] = {0x303030, 0x505050, 0x787878, 0xA0A0A0};
static const uint8_t k_noir_size[4] = {72, 86, 98, 112};  // % of the projected size
static const uint8_t k_noir_halo[4] = {0, 30, 70, 110};

static int noir_tone(int point) { return (int)((uint32_t)(point + 1) * 2654435761u >> 30); }
static float noir_speed(const face_t *f, int point) {
    return fminf(1.f, hypotf(f->tess_velocity[point][0], f->tess_velocity[point][1]) * (1.f / 14));
}

// Dimmest first, so a bright point lies over a dim one (a twinkling one over its neighbours); the keys are only
// used for the order. A twinkle takes a point from its own tone all the way to white, swells it by 1.5 px and lights
// a halo round it (r + 3 r + 2 px). Only that point's own stripes are repainted.
#define TWINKLE_SWELL_Q3 12  // 1.5 px, at a full twinkle
static void shade_fallen(const face_t *f, dot_t dots[TESS_N]) {
    for (int i = 0; i < TESS_N; i++) {
        dots[i].key = noir_tone(i) * 16384 + (int)(noir_speed(f, i) * 16000);
        dots[i].lit = 0;
    }
    for (int k = 0; k < TESS_TWINKLES; k++) {
        dot_t *dot = &dots[f->tess_twinkle_point[k]];
        int lit = (int)(tess_twinkle_level(f, k) * 256);
        if (lit > dot->lit) dot->lit = lit;
        dot->key += lit * 16;
    }
}

// A glint's four-point star: an arm of three soft dots along each axis, brightest nearest the dot, showing only
// in the top of the glint (below .5 the halo alone glints). Drawn before the dots, whose halos then lie over the roots.
static const struct { uint8_t distance_px, level; } k_star_arm[3] = {{4, 255}, {7, 160}, {10, 90}};
static void draw_star(scene_t *s, const dot_t *dot, float glow) {
    float arm = glow < .5f ? 0.f : smooth01((glow - .5f) * 2);
    if (arm <= 0) return;
    for (int step = 0; step < 3; step++) {
        int reach = k_star_arm[step].distance_px * 8, grey = (int)(k_star_arm[step].level * arm);
        uint32_t color = (uint32_t)grey * 0x010101u;
        sc_glow_dot_q3(s, dot->x + reach, dot->y, 3, color, 0);
        sc_glow_dot_q3(s, dot->x - reach, dot->y, 3, color, 0);
        sc_glow_dot_q3(s, dot->x, dot->y + reach, 3, color, 0);
        sc_glow_dot_q3(s, dot->x, dot->y - reach, 3, color, 0);
    }
}

static void draw_fallen(const face_t *f, scene_t *s, dot_t dots[TESS_N], const uint8_t order[TESS_N]) {
    for (int k = 0; k < TESS_TWINKLES; k++)
        if (f->tess_twinkle_peak[k] == TESS_GLINT_PEAK) draw_star(s, &dots[f->tess_twinkle_point[k]], tess_twinkle_level(f, k));
    for (int i = 0; i < TESS_N; i++) {
        int point = order[i], tone = noir_tone(point), lit = dots[point].lit;
        float glow = lit * (1.f / 256);
        float speed = fmaxf(noir_speed(f, point), glow);
        uint32_t color = rgb_mix(k_noir_tone[tone], 0xFFFFFF, speed);
        int radius = dots[point].radius * k_noir_size[tone] / 100 + lit * TWINKLE_SWELL_Q3 / 256;
        float halo = fmaxf(speed * .5f, glow);
        sc_glow_dot_q3(s, dots[point].x, dots[point].y, radius, color, (uint8_t)(k_noir_halo[tone] + (255 - k_noir_halo[tone]) * halo));
    }
}

static bool same_dot(const dot_t *a, const dot_t *b) {
    return a->x == b->x && a->y == b->y && a->radius == b->radius && a->key == b->key && a->lit == b->lit;
}

static uint32_t dot_hash(const dot_t *dot) {
    return (uint32_t)dot->x * 73856093u ^ (uint32_t)dot->y * 19349663u ^
           (uint32_t)dot->radius * 83492791u ^ (uint32_t)dot->key * 2654435761u ^ (uint32_t)dot->lit;
}

static int deduplicate_order(const dot_t dots[TESS_N], uint8_t order[TESS_N]) {
    uint8_t slot[256] = {0};  // 112 entries, at most 44% full
    int count = 0;
    for (int i = 0; i < TESS_N; i++) {
        unsigned bucket = dot_hash(&dots[i]) & 255u;
        while (slot[bucket] && !same_dot(&dots[slot[bucket] - 1], &dots[i])) bucket = (bucket + 1u) & 255u;
        if (!slot[bucket]) { slot[bucket] = (uint8_t)(i + 1); order[count++] = (uint8_t)i; }
    }
    return count;
}

static void sort_order(const dot_t dots[TESS_N], uint8_t order[TESS_N], int count) {
    for (int i = 1; i < count; i++) {
        int j = i;
        uint8_t index = order[i];
        while (j && dots[order[j - 1]].key > dots[index].key) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = index;
    }
}

// Adult and fallen clouds retain the original 112-dot painter. Growing or
// folding forms keep only the first exact visual duplicate.
static int cloud_order(const face_t *f, const dot_t dots[TESS_N], uint8_t order[TESS_N]) {
    bool growth = f->tess_games.ready && isfinite(f->tess_games.form) && isfinite(f->tess_games.fold) &&
                  (f->tess_games.form < 3.f || f->tess_games.fold > 0.f);
    int count;
    if (growth && !f->tess_fallen) count = deduplicate_order(dots, order);
    else {
        for (int i = 0; i < TESS_N; i++) order[i] = (uint8_t)i;
        count = TESS_N;
    }
    sort_order(dots, order, count);
    return count;
}

static void pulse_game_dots(const face_t *f, dot_t dots[TESS_N]) {
    const tess_games_t *g = &f->tess_games;
    if (!g->ready || f->tess_fallen) return;
    int scale_q8 = 256 + (int)(clampf(g->pulse, 0, 1) * .8f * 256.f + .5f);
    for (int i = 0; i < TESS_N; i++) {
        dots[i].radius = (dots[i].radius * scale_q8 + 128) >> 8;
    }
}

// Six fading positions use the projected body, not a second independently
// animated object. Sample at 18Hz; duplicate draws cannot grow the trail.
static void draw_game_trail(face_t *f, scene_t *s) {
    tess_games_t *g = &f->tess_games;
    if (g->game != TESS_GAME_CATCH || g->phase == TESS_GAME_CELEBRATE || !g->hit_valid) return;
    if (g->age - g->trail_at >= .055f) {
        for (int i = 5; i > 0; i--) memcpy(g->trail[i], g->trail[i - 1], sizeof g->trail[i]);
        memcpy(g->trail[0], g->hit, sizeof g->hit);
        if (g->trail_n < 6) g->trail_n++;
        g->trail_at = g->age;
    }
    for (int i = g->trail_n - 1; i >= 1; i--) {
        float opacity = (6 - i) / 6.f * smooth01(g->fold);
        sc_glow_dot(s, g->trail[i][0], g->trail[i][1], 2 + opacity * 3,
                    rgb_scale(0x59DCC4, opacity * .65f), (uint8_t)(opacity * 100));
    }
}

static void draw_cloud(face_t *f, scene_t *s, dot_t dots[TESS_N]) {
    uint8_t order[TESS_N];
    int count = cloud_order(f, dots, order);
    if (f->tess_fallen) {
        draw_fallen(f, s, dots, order);
        return;
    }
    const float *reaction = f->tess_reaction, *mode = f->tess_mode;
    float color_key[] = {reaction[TR_SAD], reaction[TR_ANGRY], reaction[TR_JOY],
                         reaction[TR_SHY], reaction[TR_HEART], tess_noir(f), mode[TM_SLEEP], tess_warmth(f),
                         f->feel.mix[1], f->feel.mix[2], f->feel.mix[3], f->feel.mix[4], f->feel.mix[5], f->feel.mix[6], f->feel.mix[7], reply_tint(f), game_tint(f)};
    static float previous_key[sizeof color_key / sizeof color_key[0]];
    static uint32_t shade[TESS_SHADES];
    static bool palette_valid;
    if (!palette_valid || memcmp(previous_key, color_key, sizeof color_key)) {
        for (int i = 0; i < TESS_SHADES; i++) shade[i] = tess_color(f, (i + .5f) / TESS_SHADES);
        memcpy(previous_key, color_key, sizeof color_key);
        palette_valid = true;
    }
    // Blown apart, the points are bare specks: a halo round each of 112 moving points is most of what the panel would repaint.
    int glow_q8 = (int)fminf(255.f, 230 * fmaxf(0, 1 - 2 * reaction[TR_SCATTER]) * f->style.halo);
    for (int i = 0; i < count; i++) {
        const dot_t *dot = &dots[order[i]];
        int q = dot->key * TESS_SHADES >> 16;
        // The halo grows with nearness too: far points are bare specks, near ones glow.
        uint8_t glow = (uint8_t)(q * q * glow_q8 / ((TESS_SHADES - 1) * (TESS_SHADES - 1)));
        sc_glow_dot_q3(s, dot->x, dot->y, dot->radius, shade[q], glow);
    }
}

static void draw_recording(face_t *f, scene_t *s) {
    if (f->live_active || f->tess_record <= .01f) return;
    // 18 glowing dots standing still (moving halos would repaint half the screen): the voice swells them
    // one after another round the ring.
    static float ring[18][2];
    if (!ring[0][0])
        for (int i = 0; i < 18; i++) ring[i][0] = 240 + 150 * cosf(i * 2 * PI / 18), ring[i][1] = 255 + 150 * sinf(i * 2 * PI / 18);
    for (int i = 0; i < 18; i++) {
        float swell = f->tess_audio * (.5f + .5f * tess_sin(i * (2 * PI / 6) + f->t * 3));
        sc_glow_dot(s, ring[i][0], ring[i][1], (3.2f + 1.6f * swell) * f->tess_record, COL_REC,
                    (uint8_t)(230 * f->tess_record));
    }
}

void tess_project_dots(face_t *f, tess_projected_t dots[TESS_N]) {
    float initial[TESS_N][3], m[3][3];
    const float (*points)[3] = f->tess_position;
    if (!f->tess_points_ready) {
        tess_point_targets(f, initial, -1);
        points = initial;
    }
    tess_view_matrix(f, m);
    if (f->tess_fallen)  // fallen points already lie in screen space
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) m[i][j] = i == j;
    float heart = f->tess_reaction[TR_HEART];
    // The 4D depth counts only while it is a tesseract (not the globe, the heart or the scatter).
    float listen = f->live_active ? 0 : f->tess_mode[TM_LISTEN];
    float form = f->tess_fallen ? 0 : 1 - fmaxf(fmaxf(fmaxf(listen, heart), f->tess_reaction[TR_SCATTER]), f->tess_drift);
    form *= 1 - f->tess_mode[TM_OFFLINE];
    float wgain = .9f * form, dust = (1 - .35f * f->tess_drift - .4f * f->tess_reaction[TR_SCATTER]) * f->style.dot;
    float camera = 4.7f + (f->style.cam - 4.7f) * form;  // (a mood's camera is for the tesseract only)
    float depth_range = 1.7f + .6f * form - heart * .7f, depth_gain = 1 / (2 * depth_range);
    project_points(f, points, m, camera, wgain, dust, depth_range, depth_gain, dots);
    fit_conversation(f, dots);
    if (!f->tess_games.ready || f->tess_fallen) {
        f->tess_games.hit_valid = false;
    } else {
        int64_t x = 0, y = 0;
        for (int i = 0; i < TESS_N; i++) { x += dots[i].x; y += dots[i].y; }
        f->tess_games.hit[0] = (float)(x / TESS_N) * (1.f / 8.f);
        f->tess_games.hit[1] = (float)(y / TESS_N) * (1.f / 8.f);
        f->tess_games.hit_valid = true;
    }
}

void tess_draw(face_t *f, scene_t *s) {
    dot_t dots[TESS_N];
    tess_project_dots(f, dots);
    pulse_game_dots(f, dots);
    if (f->tess_fallen) shade_fallen(f, dots);
    draw_game_trail(f, s);
    draw_cloud(f, s, dots);
    if (f->tess_games.game == TESS_GAME_CATCH && f->tess_games.phase != TESS_GAME_CELEBRATE &&
        f->tess_games.hit_valid && f->tess_games.fold > .8f) {
        float pip = smooth01((f->tess_games.fold - .8f) * 5.f);
        sc_glow_dot(s, f->tess_games.hit[0], f->tess_games.hit[1], 13 * pip,
                    rgb_mix(0x59DCC4, 0xFFCE78, game_tint(f)), 200);
    }
    draw_recording(f, s);
}
