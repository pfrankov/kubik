// Tess is a rigid body: only turns move it (3D planes and x-w, y-w, z-w), seen in perspective from a camera that
// may look from any side, and its size may change only as a whole. Before any projection the 16 vertices must keep
// every pairwise 4D distance of the unit tesseract, in every frame of every mode; and while it stands as the
// tesseract its edges stay straight and evenly spaced. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rub_script.h"
#include "../main/tess_internal.h"

#define DT (1.f / 30)
static unsigned frames_checked, rigid_frames;
static float worst_distance, worst_edge;

static void check_vertices(const face_t *f) {
    float q[16][4];
    tess_vertices4d(f, q);
    for (int a = 0; a < 16; a++)
        for (int b = a + 1; b < 16; b++) {
            float d = 0, want = 4.f * __builtin_popcount(a ^ b);  // each differing coordinate is 2 apart
            for (int k = 0; k < 4; k++) d += (q[a][k] - q[b][k]) * (q[a][k] - q[b][k]);
            worst_distance = fmaxf(worst_distance, fabsf(d - want));
        }
}

// The point order of tess_form: a vertex, then three points along each of its edges to a higher vertex.
static void check_edges(const face_t *f) {
    int vertex_slot[16], edge_from[TESS_N], edge_bit[TESS_N], n = 0;
    for (int i = 0; i < 16; i++) {
        vertex_slot[i] = n; edge_from[n] = -1; edge_bit[n++] = 0;
        for (int bit = 1; bit <= 8; bit <<= 1) {
            if (i & bit) continue;
            for (int k = 0; k < 3; k++) { edge_from[n] = i; edge_bit[n++] = bit; }
        }
    }
    int at[TESS_N];  // the point that holds each slot
    for (int i = 0; i < TESS_N; i++) at[f->tess_form_of[i]] = i;
    for (int s = 0; s < TESS_N; s++) {
        if (edge_from[s] < 0) continue;
        int k = 1 + (s - vertex_slot[edge_from[s]] - 1) % 3;  // 1..3 of 4 along the edge
        const float *a = f->tess_position[at[vertex_slot[edge_from[s]]]], *b = f->tess_position[at[vertex_slot[edge_from[s] | edge_bit[s]]]];
        for (int axis = 0; axis < 3; axis++)
            worst_edge = fmaxf(worst_edge, fabsf(f->tess_position[at[s]][axis] - (a[axis] + (b[axis] - a[axis]) * k * .25f)));
    }
}

static void step(face_t *f) {
    face_update(f, DT);
    check_vertices(f);
    frames_checked++;
    if (f->tess_rigid >= 1) { check_edges(f); rigid_frames++; }
}

static face_t start(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    return f;
}

int main(void) {
    static const face_mode_t modes[] = {MODE_IDLE, MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_SLEEP, MODE_OFFLINE, MODE_IDLE};
    static const face_event_t events[] = {FEV_TAP, FEV_PET, FEV_PICKUP, FEV_SHAKE, FEV_NOTIFY, FEV_NOT_HEARD, FEV_FAIL, FEV_WAKE};
    face_t f = start();
    for (int m = 0; m < 7; m++) {  // every mode for 12 s, with a gesture now and then and the voice going
        face_set_mode(&f, modes[m]);
        for (int i = 0; i < 12 * 30; i++) {
            f.mic_level = f.spk_level = .5f + .5f * sinf(i * .2f);
            f.view_q[0] = cosf(i * .01f); f.view_q[1] = sinf(i * .01f); f.view_q[2] = f.view_q[3] = 0;
            if (i % 90 == 45) face_event(&f, events[(i / 90 + m) % 8], 100 + i % 300, 200);
            step(&f);
        }
    }
    for (int e = 0; e < EMO_COUNT; e++) {  // every emotion
        face_set_emotion(&f, (emotion_t)e, 2.f);
        for (int i = 0; i < 90; i++) step(&f);
    }
    for (int mood = 0; mood < MOOD_COUNT; mood++) {  // every mood, held: the body is only turned, carried and sized whole
        f = start();
        if (mood == MOOD_SLEEPY) face_set_emotion(&f, EMO_SLEEPY, -1);
        face_force_mood(&f, (mood_t)mood, 1.f);
        for (int i = 0; i < 12 * 30; i++) {
            if (i % 90 == 45) face_event(&f, events[(i / 90 + mood) % 8], 100 + i % 300, 200);
            step(&f);
        }
    }
    f = start();
    rub_finger_t finger = {0};
    for (int i = 0; i < 12 * 30; i++) {  // rubbed all the way to the heart
        rub_script_feed(&f, &finger, SCRIPT_VIGOROUS, i * DT);
        step(&f);
    }
    printf("tess rigidity: %u frames, %u standing as the tesseract; 4D distances off by %.1e, edges off by %.1e\n",
           frames_checked, rigid_frames, worst_distance, worst_edge);
    assert(worst_distance < 2e-4f);
    assert(rigid_frames > frames_checked / 4 && worst_edge < 1e-3f);
    puts("tess rigid ok");
    return 0;
}
