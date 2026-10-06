#include "face_internal.h"

// ---------------------------------------------------------------- drawing

uint32_t face_color(face_t *f) {
    float *v = f->cur.v;
    uint32_t c = ((uint32_t)clampf(v[P_CR], 0, 255) << 16) | ((uint32_t)clampf(v[P_CG], 0, 255) << 8) |
                 (uint32_t)clampf(v[P_CB], 0, 255);
    return rgb_scale(c, clampf(v[P_DIM], 0.2f, 1.2f));
}

static void draw_zed(scene_t *s, float x, float y, float sz, uint32_t c, float a) {
    float h = sz * 0.5f, th = sz * 0.2f;
    sc_capsule(s, x - h, y - h, x + h, y - h, th, c, a);
    sc_capsule(s, x + h, y - h, x - h, y + h, th, c, a);
    sc_capsule(s, x - h, y + h, x + h, y + h, th, c, a);
}

static void draw_sparkle(scene_t *s, float x, float y, float sz, uint32_t c, float a) {
    sc_capsule(s, x - sz, y, x + sz, y, sz * 0.22f, c, a);
    sc_capsule(s, x, y - sz * 1.3f, x, y + sz * 1.3f, sz * 0.22f, c, a);
    sc_circle(s, x, y, sz * 0.3f, c, a);
}

void draw_wifi(scene_t *s, float x, float y, uint32_t c, float a, float phase) {
    sc_circle(s, x, y, 6, c, a);
    for (int i = 0; i < 3; i++) {
        float lit = phase < 0 ? 1 : clampf(sinf(phase - i * 0.9f) * 0.5f + 0.6f, 0.25f, 1);
        sc_arc(s, x, y, 17 + i * 15, 0.78f, 7, PI, c, a * lit);
    }
}

// Device and server rack separated by a broken link. Wi-Fi itself is already
// connected in this mode; the server has not completed its welcome handshake.
static void draw_server_disconnected(scene_t *s, float x, float y, uint32_t c, float a) {
    float cy = y - 19;
    sc_rbox(s, x - 36, cy, 12, 12, 4, 0, c, a);
    sc_rbox(s, x - 36, cy, 6, 6, 1, 0, 0, 1);
    sc_rbox(s, x + 35, cy, 15, 22, 4, 0, c, a);
    sc_rbox(s, x + 35, cy, 9, 16, 2, 0, 0, 1);
    sc_capsule(s, x + 30, cy - 6, x + 40, cy - 6, 4, c, a);
    sc_capsule(s, x + 30, cy + 6, x + 40, cy + 6, 4, c, a);
    sc_capsule(s, x - 23, cy, x - 8, cy, 6, c, a);
    sc_capsule(s, x + 8, cy, x + 20, cy, 6, c, a);
    sc_capsule(s, x - 4, cy - 10, x + 4, cy + 10, 5, COL_REC, a);
}

static void draw_eye_shape(scene_t *s, float ex, float ey, float w, float h, float r, float tilt,
                           float top, float eye_a, float smile, float lookx, float looky,
                           float ang, float bot, uint32_t col) {
    if (top > 0.96f) {
        // Fully shut: a gentle "∩" where the lids meet.
        sc_arc(s, ex, ey + h * 0.95f, w * 1.1f, 0.85f, 12, tilt + PI, col, eye_a);
        return;
    }
    // Big smiles turn the eye into a thick "∩" (the ^^ look); crossfade from the carved crescent.
    float arc_k = smooth01((smile - 0.62f) / 0.2f) * (1 - top);
    if (arc_k > 0.01f) sc_arc(s, ex, ey + h * 0.55f, w * 0.95f, 1.2f, 24, tilt + PI, col, eye_a * arc_k);
    eye_a *= 1 - arc_k;
    if (eye_a < 0.02f) return;
    prim_t *e = sc_rbox(s, ex, ey, w, h, r, tilt, col, eye_a);
    if (!e) return;
    // Upper lid: a tilted half-plane; positive angle drops the inner corner.
    float ly = -h + top * 2 * h;
    float nx = sinf(ang), ny = -cosf(ang);
    if (top > 0.005f || fabsf(ang) > 0.01f) pr_cut(e, nx, ny, ny * ly);
    if (bot > 0.005f) pr_cut(e, 0, 1, h - bot * 2 * h);
    // Happy eyes: a circle rises from below and carves a crescent.
    if (smile > 0.01f) {
        float R = w * 1.55f;
        pr_sub_circle(e, 0, h - smile * 1.55f * h + R, R);
    }
    // Glossy highlights, clipped to the eye, drifting slightly against the gaze.
    float hx = -lookx * 0.12f, hy = -looky * 0.1f;
    prim_t *h1 = sc_circle(s, ex + w * 0.34f + hx, ey - h * 0.40f + hy, w * 0.27f, COL_WHITE, eye_a * 0.92f);
    pr_clip(s, h1, e);
    prim_t *h2 = sc_circle(s, ex - w * 0.30f + hx, ey + h * 0.36f + hy, w * 0.12f, COL_WHITE, eye_a * 0.55f);
    pr_clip(s, h2, e);
}

static void draw_eye(face_t *f, scene_t *s, int side, uint32_t col) {
    float *v = f->cur.v;
    float sgn = side ? 1.f : -1.f;
    float tilt = v[P_TILT];
    float sc = side ? v[P_SCR] : v[P_SCL];
    float lookx = v[P_LOOKX], looky = v[P_LOOKY];
    float squash = 1.f + clampf(-f->hop_v * 0.0006f, -0.08f, 0.08f);
    float bob = has_body(f) ? 0.f : sinf(f->t * 2 * PI / 4.2f) * 2.5f + f->hop;  // the body carries the hop
    float px = sgn * v[P_SEP] * 0.5f + lookx;
    float py = v[P_EY] + looky * 0.8f + bob - 240.f;
    float ex = CX + px * cosf(tilt) - py * sinf(tilt);
    float ey = 240.f + px * sinf(tilt) + py * cosf(tilt);
    // Looking sideways makes the far eye a touch smaller.
    float persp = 1.f + sgn * lookx * 0.0035f;
    float w = v[P_EW] * sc * persp, h = v[P_EH] * sc * squash;
    float r = fminf(v[P_ER] * sc, fminf(w, h));

    float dizzy = f->dizzy_t > 0 || f->emotion == EMO_DIZZY;
    if (dizzy) {
        float rot = f->t * 7 * (side ? 1 : -1);
        sc_arc(s, ex, ey, 34, 1.25f, 8, rot, col, 1);
        sc_arc(s, ex, ey, 16, 1.1f, 7, rot + PI, col, 1);
        sc_circle(s, ex, ey, 5, col, 1);
        return;
    }

    float sleep = clampf(v[P_SLEEPEYE], 0, 1);
    float heart = clampf(v[P_HEART], 0, 1);
    float top = clampf(fmaxf(side ? v[P_LTR] : v[P_LTL], f->blink), 0, 1);
    float eye_a = (1 - heart) * (1 - sleep);

    if (sleep > 0.02f) sc_arc(s, ex, ey + 20, 30, 0.95f, 8, tilt, col, sleep);
    if (heart > 0.02f) {
        float pulse = 1 + 0.06f * sinf(f->t * 9);
        prim_t *hp = sc_heart(s, ex, ey + 4, 118 * pulse * sc, tilt + sgn * 0.08f, rgb_mix(col, COL_PINK, 0.4f), heart);
        (void)hp;
        sc_circle(s, ex + 22, ey - 18, 9, COL_WHITE, heart * 0.85f);
    }
    if (eye_a < 0.02f) return;

    float smile = clampf(v[P_SMILE], 0, 1);
    float ang = (side ? v[P_LAR] : v[P_LAL]) * -sgn;
    float bot = clampf(v[P_LBOT], 0, 1);
    draw_eye_shape(s, ex, ey, w, h, r, tilt, top, eye_a, smile, lookx, looky, ang, bot, col);
}

static void draw_mouth(face_t *f, scene_t *s, uint32_t col) {
    float *v = f->cur.v;
    float bob = has_body(f) ? 0.f : sinf(f->t * 2 * PI / 4.2f) * 3.f + f->hop * 1.15f;
    float tilt = v[P_TILT];
    float mx0 = v[P_MDX] + v[P_LOOKX] * 0.45f;
    float my0 = 330.f - 240.f + v[P_LOOKY] * 0.45f + bob + (v[P_EY] - 212.f) * 0.5f;
    float mx = CX + mx0 * cosf(tilt) - my0 * sinf(tilt);
    float my = 240.f + mx0 * sinf(tilt) + my0 * cosf(tilt);

    if (f->mode == MODE_LISTENING) {
        // Live equaliser instead of the mouth: the user sees they are heard. The middle
        // follows the voice now, the sides echo it a moment later.
        static const uint8_t hist[7] = {5, 3, 1, 0, 1, 3, 5};
        static const float env[7] = {0.45f, 0.7f, 0.9f, 1.f, 0.9f, 0.7f, 0.45f};
        for (int i = 0; i < 7; i++) {
            float l = clampf(f->mic_hist[hist[i]] * 1.3f, 0, 1);
            float wob = 0.5f + 0.5f * sinf(f->t * (8.f + i * 1.3f) + i * 2.1f);
            float hh = 8 + clampf(l * env[i] * (0.7f + 0.3f * wob), 0, 1) * 78 + 4 * wob;
            float x = mx + (i - 3) * 25;
            sc_capsule(s, x, my + 6 - hh * 0.5f, x, my + 6 + hh * 0.5f, 14, col, 0.7f + 0.3f * env[i]);
        }
        return;
    }
    float open = clampf(v[P_MOPEN], 0, 1.2f);
    float o = clampf(v[P_MO], 0, 1.5f);
    float curve = clampf(v[P_MCURVE], -1, 1);
    float w = clampf(v[P_MW], 6, 44) * 1.15f;
    if (o > 0.05f) {
        float rr = 7 + 7 * o + open * 6;
        sc_ring(s, mx, my + 2, rr, 7, col, clampf(o, 0, 1));
        if (o > 0.9f) sc_circle(s, mx, my + 2, rr - 3, rgb_scale(col, 0.25f), 1);
        return;
    }
    if (open > 0.06f) {
        float hw = w * 0.75f + 6 + open * 6;
        float hh = 4 + open * 20;
        prim_t *m = sc_rbox(s, mx, my + hh * 0.35f, hw, hh, fminf(hw, hh), tilt, col, 1);
        if (m && curve > 0.3f) pr_cut(m, 0, -1, hh * 0.45f);  // flat top: laughing "D" mouth
        if (open > 0.35f) {
            prim_t *tg = sc_circle(s, mx, my + hh * 1.05f, hw * 0.55f, 0xFF7A9Au, 1);
            pr_clip(s, tg, m);
        }
        return;
    }
    if (fabsf(curve) < 0.1f) {
        sc_capsule(s, mx - w, my, mx + w, my, 9, col, 1);
        return;
    }
    float ap = 0.3f + 0.75f * fabsf(curve);
    float R = w / sinf(ap);
    if (curve > 0) sc_arc(s, mx, my - R * cosf(ap), R, ap, 9, tilt, col, 1);
    else sc_arc(s, mx, my + R * cosf(ap) + 6, R, ap, 9, tilt + PI, col, 1);
}

// Where overlays hang around the head (panel pixels).
typedef struct {
    float x, y;    // head centre (face screen centre)
    float hw;      // head half width
    float top;     // top of the head
} anchor_t;

static anchor_t head_anchor(const face_t *f) {
    anchor_t a = {CX, 240.f, 200.f, 20.f};
    if (has_body(f)) {
        a.x = f->scr_cx;
        a.y = f->scr_cy;
        a.hw = f->scr_hw * 1.5f;
        a.top = f->scr_cy - f->scr_hh * 1.75f;
    }
    return a;
}

// Listening: a red outline along the screen's rounded edge grows from a hairline to a
// solid band; when the phrase leaves it shrinks back
// and a few sparks fly up. One hollow shape: its inside is skipped by the rasteriser.
#define SCREEN_R 84.f   // corner radius of the panel's visible area, px
#define REC_GROW 0.3f   // s from a hairline to the full band
#define REC_W 14.f      // band width, px (even: whole native pixels)
static void draw_listening(face_t *f, scene_t *s, uint32_t col, anchor_t h, bool body) {
    float w = 0;
    if (f->mode == MODE_LISTENING) {
        float e = smooth01(f->mode_t / REC_GROW);  // starts as a hairline, eases into the band
        w = 1 + (REC_W - 1) * e;  // then still: a static edge stays out of the canvas diff (no full-screen transfer)
    } else if (f->sent_t >= 0 && f->sent_t < 0.25f) {
        w = REC_W * (1 - smooth01(f->sent_t / 0.25f));
    }
    // Drawn on the panel rows, outside the canvas: while it holds still it costs nothing.
    int iw = (int)lrintf(w);
    if (iw >= 1) sc_edge(s, iw, (int)SCREEN_R, COL_REC);

    if (f->sent_t >= 0 && f->mode == MODE_THINKING) {
        float k = f->sent_t / 0.8f, top = body ? h.top : 60.f;
        for (int i = 0; i < 3; i++) {  // the phrase flies off, upwards
            float u = clampf((k - 0.1f - i * 0.08f) / 0.6f, 0, 1);
            if (u <= 0 || u >= 1) continue;
            float x = h.x + (i - 1) * 18 * (1 - u * 0.5f), y = top + 10 - u * 70;
            sc_circle(s, x, y, 6 - 3 * u, col, 1 - u);
        }
    }
}

static void draw_state_overlays(face_t *f, scene_t *s, uint32_t col, anchor_t h, bool body) {
    if (f->mode == MODE_THINKING) {
        float bx = body ? h.x + h.hw * 0.72f : 330.f, by = body ? h.top + 8.f : 124.f;
        float st = body ? 20.f : 27.f;
        for (int i = 0; i < 3; i++) {
            float ph = f->mode_t * 3.2f - i * 0.8f;
            float k = 0.5f + 0.5f * sinf(ph);
            sc_circle(s, bx + i * st, by - i * st, (4 + i * 3.f) * (0.8f + 0.35f * k), col, 0.35f + 0.65f * k);
        }
    }
    if (!body) {
        if (f->mode == MODE_SETUP) draw_wifi(s, CX, 96, col, 1, f->t * 3);
        if (f->mode == MODE_OFFLINE) draw_server_disconnected(s, CX, 96, COL_GREY, 0.8f);
    }
    if (f->emotion == EMO_CONFUSED && f->mode != MODE_LISTENING) {
        float bx = body ? h.x + h.hw * 0.95f : 382.f, by = (body ? h.top + 14.f : 104.f) + sinf(f->t * 4) * 3;
        sc_arc(s, bx, by - 14, 12, 1.9f, 7, PI + 0.5f, col, 1);
        sc_capsule(s, bx + 3, by + 2, bx, by + 12, 7, col, 1);
        sc_circle(s, bx, by + 26, 4.5f, col, 1);
    }
}

static void draw_status_overlays(face_t *f, scene_t *s, uint32_t col, bool body) {
    if (f->notify_t > 0 && !body) {
        float k = f->notify_t / 1.2f;
        sc_ring(s, CX, 240, 150 + (1 - k) * 70, 6, col, k * 0.7f);
    }
    if (!body && f->volume_show > 0) {
        float a = clampf(f->volume_show * 2, 0, 1);
        int lit = (int)(f->volume_level * 5 + 0.5f);
        for (int i = 0; i < 5; i++)
            sc_circle(s, CX + (i - 2) * 26, 418, i < lit ? 8 : 5, i < lit ? col : COL_GREY, a * (i < lit ? 1 : 0.6f));
    }
}

static void draw_particles(face_t *f, scene_t *s, uint32_t col) {
    for (int i = 0; i < FACE_MAX_PARTICLES; i++) {
        particle_t *p = &f->parts[i];
        if (p->life <= 0) continue;
        float k = p->life / p->max_life;
        float a = clampf(k * 2.5f, 0, 1) * clampf((1 - k) * 6, 0, 1);
        if (p->kind == 0) sc_heart(s, p->x, p->y, p->size, p->spin * 0.4f, COL_PINK, a);
        else if (p->kind == 1) draw_sparkle(s, p->x, p->y, p->size * (0.4f + 0.6f * sinf(k * PI)), COL_GOLD, a);
        else draw_zed(s, p->x, p->y, p->size * (1.6f - k * 0.6f), col, a * 0.8f);
    }
}

static void draw_overlays(face_t *f, scene_t *s, uint32_t col) {
    anchor_t h = head_anchor(f);
    bool body = has_body(f);
    if (f->mode == MODE_LISTENING || f->sent_t >= 0) draw_listening(f, s, col, h, body);
    draw_state_overlays(f, s, col, h, body);
    draw_status_overlays(f, s, col, body);
    draw_particles(f, s, col);
}

// Status shown on the face screen itself (body mode): Wi-Fi setup, server link, volume, battery.
static bool draw_screen_status(face_t *f, scene_t *s, uint32_t col) {
    bool mouth_taken = false;
    if (f->mode == MODE_SETUP) {
        draw_wifi(s, CX, 350, col, 1, f->t * 3);
        mouth_taken = true;
    }
    if (f->mode == MODE_OFFLINE) {
        draw_server_disconnected(s, CX, 350, COL_GREY, 0.8f);
        mouth_taken = true;
    }
    if (f->volume_show > 0 && !mouth_taken) {
        float a = clampf(f->volume_show * 2, 0, 1);
        int lit = (int)(f->volume_level * 5 + 0.5f);
        for (int i = 0; i < 5; i++)
            sc_circle(s, CX + (i - 2) * 34, 335, i < lit ? 12 : 7, i < lit ? col : COL_GREY, a * (i < lit ? 1 : 0.6f));
        mouth_taken = true;
    }
    if (f->notify_t > 0) {  // a ripple across the glass
        float k = f->notify_t / 1.2f;
        sc_ring(s, CX, 262, 40 + (1 - k) * 220, 14, col, k * 0.8f);
    }
    return mouth_taken;
}

void face_plush_draw(face_t *f, scene_t *s, uint32_t col) {
    float *v = f->cur.v;
    bool body = has_body(f);

    if (body) {
        // The plush body, then the face mapped onto its screen (clipped to the glass).
        const body_t *b = &f->body;
        sprite_breathe(sc_sprite(s, b->anim, b->frame, (int)lrintf(f->body_dx), (int)lrintf(f->body_dy), 1.f), f->body_breath);
        float vis = smooth01((f->scr_vis - 0.3f) / 0.4f);
        float k = fminf(f->scr_hw / 185.f, f->scr_hh / 135.f);
        prim_t *m;
        // Loose face: shifting the design origin moves the face within the glass (it stays
        // in the screen's perspective); a squash flattens it against the edge it hit.
        float ox = CX - f->loose_x, oy = 262.f - f->loose_y;
        float sx = 1.f + f->squash_x, sy = 1.f + f->squash_y;
        if (f->scr_has_quad) {
            // Perspective: the design rectangle goes exactly where the tracked corners say;
            // the glass mask rides along (design units: the screen is centred on the face).
            scene_xform_quad(s, ox, oy, 185.f * sx, 135.f * sy, f->scr_quad);
            m = sc_mask_rbox(s, ox, oy, (f->scr_hw - 3) / k * sx, (f->scr_hh - 3) / k * sy,
                             fminf(f->scr_hw, f->scr_hh) * 0.42f / k * fminf(sx, sy), 0.f);
        } else {
            m = sc_mask_rbox(s, f->scr_cx, f->scr_cy, f->scr_hw - 3, f->scr_hh - 3,
                             fminf(f->scr_hw, f->scr_hh) * 0.42f, f->scr_ang);
            scene_xform(s, ox, oy, f->scr_cx, f->scr_cy, k, f->scr_ang);
        }
        s->xf_alpha = vis;
        scene_clip_all(s, m);
        scene_glass_begin(s, f->scr_has_glass ? f->scr_gx : 0.f, f->scr_gy, f->scr_has_glass ? f->scr_gr : NULL,
                          SPRITE_GLASS_N);
    }

    // Cheeks first so everything else sits on top of the blush.
    float cheek = clampf(v[P_CHEEK], 0, 1);
    if (cheek > 0.02f) {
        float bob = body ? 0.f : f->hop * 0.9f;
        for (int i = -1; i <= 1; i += 2) {
            prim_t *c = sc_rbox(s, CX + i * 152 + v[P_LOOKX] * 0.5f, 296 + v[P_LOOKY] * 0.4f + bob, 30, 17, 17, 0,
                                COL_CHEEK, cheek * 0.7f);
            pr_soft(c, 16);
        }
    }
    draw_eye(f, s, 0, col);
    draw_eye(f, s, 1, col);
    bool mouth_taken = body ? draw_screen_status(f, s, col) : false;
    if (!mouth_taken) draw_mouth(f, s, col);

    if (body) {
        scene_glass_end(s);
        scene_clip_all(s, NULL);
        scene_xform_off(s);
    }
    draw_overlays(f, s, col);
    draw_cron(f, s);
    draw_card(f, s);
    draw_battery(f, s, false);
    draw_menu_fade(f, s);
    draw_bubble(f, s);
}
