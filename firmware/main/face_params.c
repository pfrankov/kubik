#include "face_internal.h"

// ---------------------------------------------------------------- helpers

static void set_color(face_params_t *p, uint32_t rgb) {
    p->v[P_CR] = (rgb >> 16) & 255;
    p->v[P_CG] = (rgb >> 8) & 255;
    p->v[P_CB] = rgb & 255;
}

void base_params(face_params_t *p) {
    memset(p, 0, sizeof(*p));
    p->v[P_EW] = 54;
    p->v[P_EH] = 72;
    p->v[P_ER] = 38;
    p->v[P_SEP] = 190;
    p->v[P_EY] = 212;
    p->v[P_SCL] = 1;
    p->v[P_SCR] = 1;
    p->v[P_MCURVE] = 0.35f;
    p->v[P_MW] = 24;
    p->v[P_CHEEK] = 0.18f;
    p->v[P_DIM] = 1;
    set_color(p, COL_MINT);
}

static void emotion_params(face_params_t *p, emotion_t e) {
    switch (e) {
    case EMO_HAPPY:
        p->v[P_SMILE] = 0.85f;
        p->v[P_EH] = 68;
        p->v[P_MCURVE] = 0.9f;
        p->v[P_MW] = 28;
        p->v[P_CHEEK] = 0.7f;
        break;
    case EMO_JOY:
        p->v[P_SMILE] = 1.0f;
        p->v[P_EH] = 66;
        p->v[P_MCURVE] = 1.0f;
        p->v[P_MW] = 30;
        p->v[P_MOPEN] = 0.65f;
        p->v[P_CHEEK] = 0.95f;
        break;
    case EMO_LOVE:
        p->v[P_HEART] = 1;
        p->v[P_MCURVE] = 0.8f;
        p->v[P_MW] = 22;
        p->v[P_CHEEK] = 1;
        set_color(p, COL_PINK);
        break;
    case EMO_SAD:
        p->v[P_EH] = 62;
        p->v[P_LTL] = p->v[P_LTR] = 0.28f;
        p->v[P_LAL] = p->v[P_LAR] = -0.42f;
        p->v[P_EY] = 222;
        p->v[P_LOOKY] = 10;
        p->v[P_MCURVE] = -0.65f;
        p->v[P_MW] = 20;
        p->v[P_CHEEK] = 0;
        set_color(p, COL_BLUE);
        break;
    case EMO_ANGRY:
        p->v[P_EH] = 64;
        p->v[P_LTL] = p->v[P_LTR] = 0.34f;
        p->v[P_LAL] = p->v[P_LAR] = 0.5f;
        p->v[P_MCURVE] = -0.35f;
        p->v[P_MW] = 18;
        p->v[P_CHEEK] = 0;
        set_color(p, COL_ORANGE);
        break;
    case EMO_SURPRISED:
        p->v[P_EW] = 62;
        p->v[P_EH] = 80;
        p->v[P_ER] = 60;
        p->v[P_EY] = 202;
        p->v[P_MO] = 1;
        p->v[P_MCURVE] = 0;
        p->v[P_CHEEK] = 0;
        break;
    case EMO_CONFUSED:
        p->v[P_SCR] = 0.82f;
        p->v[P_LTR] = 0.3f;
        p->v[P_LAR] = -0.3f;
        p->v[P_TILT] = 0.13f;
        p->v[P_LOOKX] = -8;
        p->v[P_LOOKY] = -6;
        p->v[P_MCURVE] = -0.15f;
        p->v[P_MW] = 14;
        p->v[P_MDX] = 10;
        break;
    case EMO_SLEEPY:
        p->v[P_LTL] = p->v[P_LTR] = 0.55f;
        p->v[P_LBOT] = 0.08f;
        p->v[P_LOOKY] = 8;
        p->v[P_MCURVE] = 0.1f;
        p->v[P_MW] = 12;
        p->v[P_DIM] = 0.75f;
        break;
    case EMO_THINKING:
        p->v[P_LOOKX] = 20;
        p->v[P_LOOKY] = -20;
        p->v[P_LTL] = 0.12f;
        p->v[P_LTR] = 0.24f;
        p->v[P_LAR] = -0.2f;
        p->v[P_MCURVE] = 0.0f;
        p->v[P_MW] = 12;
        p->v[P_MDX] = 16;
        p->v[P_CHEEK] = 0.1f;
        break;
    case EMO_WINK:
        p->v[P_SMILE] = 0.55f;
        p->v[P_LTR] = 1.0f;
        p->v[P_MCURVE] = 0.9f;
        p->v[P_MW] = 26;
        p->v[P_CHEEK] = 0.7f;
        break;
    case EMO_SHY:
        p->v[P_SMILE] = 0.35f;
        p->v[P_LOOKX] = -16;
        p->v[P_LOOKY] = 16;
        p->v[P_MCURVE] = 0.55f;
        p->v[P_MW] = 13;
        p->v[P_CHEEK] = 1;
        break;
    case EMO_PROUD:
        p->v[P_SMILE] = 0.5f;
        p->v[P_EY] = 200;
        p->v[P_LOOKY] = -8;
        p->v[P_MCURVE] = 0.85f;
        p->v[P_MW] = 26;
        p->v[P_CHEEK] = 0.5f;
        set_color(p, COL_GOLD);
        break;
    default:
        break;
    }
}

static void selected_emotion_params(face_params_t *p, emotion_t e) {
    if (e == EMO_DIZZY) {
        p->v[P_MCURVE] = -0.2f;
        p->v[P_MW] = 16;
        p->v[P_MO] = 0.5f;
    } else {
        emotion_params(p, e);
    }
}

static void mode_params(face_t *f, face_params_t *p, emotion_t e) {
    switch (f->mode) {
    case MODE_LISTENING:
        // Attentive: slightly bigger open eyes, looking at the user. Emotion colour kept.
        selected_emotion_params(p, e == EMO_LOVE || e == EMO_SAD || e == EMO_ANGRY ? EMO_NEUTRAL : e);
        p->v[P_SMILE] *= 0.3f;
        p->v[P_EH] = 78 + f->listen_lvl * 10;  // eyes widen as the voice gets louder
        p->v[P_EW] = 56 + f->listen_lvl * 4;
        p->v[P_ER] = 42;
        p->v[P_LTL] = p->v[P_LTR] = 0;
        p->v[P_LAL] = p->v[P_LAR] = 0;
        p->v[P_TILT] = 0.05f;
        p->v[P_CHEEK] = 0.35f;
        p->v[P_MO] = 0;
        break;
    case MODE_THINKING:
        selected_emotion_params(p, EMO_THINKING);
        break;
    case MODE_SLEEP:
        p->v[P_SLEEPEYE] = 1;
        p->v[P_LOOKY] = 12;
        p->v[P_MCURVE] = 0.15f;
        p->v[P_MW] = 10;
        p->v[P_DIM] = 0.55f;
        p->v[P_CHEEK] = 0.25f;
        break;
    case MODE_SETUP:
        p->v[P_LOOKY] = -22;
        p->v[P_EH] = 76;
        p->v[P_MO] = 0.35f;
        break;
    case MODE_OFFLINE:
        p->v[P_LTL] = p->v[P_LTR] = 0.42f;
        p->v[P_LOOKY] = 6;
        p->v[P_MCURVE] = -0.2f;
        p->v[P_MW] = 14;
        p->v[P_CHEEK] = 0;
        set_color(p, COL_GREY);
        break;
    case MODE_BOOT: {
        float k = smooth01((f->boot_t - 0.25f) / 0.6f);
        p->v[P_LTL] = p->v[P_LTR] = 1 - k;
        p->v[P_MW] = 8 + 12 * k;
        break;
    }
    default:
        selected_emotion_params(p, e);
        break;
    }
}

static void micro_action_params(face_t *f, face_params_t *p) {
    // Micro-actions while idle.
    if (f->mode == MODE_IDLE && f->micro_left > 0) {
        switch (f->micro) {
        case 0:  // look around
            p->v[P_LOOKX] = f->micro_left > 1.1f ? -30 : 30;
            break;
        case 1:  // curious tilt
            p->v[P_TILT] = 0.16f;
            p->v[P_LOOKY] = -6;
            p->v[P_SCR] = 1.06f;
            break;
        case 2:  // content smile
            p->v[P_SMILE] = 0.55f;
            p->v[P_MCURVE] = 0.8f;
            p->v[P_CHEEK] = 0.6f;
            break;
        case 3:  // yawn
            p->v[P_MO] = 1.4f;
            p->v[P_LTL] = p->v[P_LTR] = 0.5f;
            p->v[P_LOOKY] = -6;
            break;
        }
    }
}

void compute_target(face_t *f) {
    face_params_t *p = &f->target;
    base_params(p);
    mode_params(f, p, f->emotion);
    micro_action_params(f, p);
    face_rub_params(f, p);
    // Poked eyes squeeze shut briefly.
    if (f->poke_eye_t[0] > 0) p->v[P_LTL] = 1;
    if (f->poke_eye_t[1] > 0) p->v[P_LTR] = 1;
}

