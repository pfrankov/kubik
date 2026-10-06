// Audible preview driven by the actual offline physics, not a scheduled melody.
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "../main/face.h"
#include "../main/tess_sound.h"

static void little(FILE *file, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) fputc((value >> (i * 8)) & 255, file);
}
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    FILE *file = fopen(argv[1], "wb");
    if (!file) return 1;
    enum { FRAMES = 30 * 16, SAMPLES = AUDIO_RATE / 30 };
    uint32_t size = FRAMES * SAMPLES * 2;
    fwrite("RIFF", 1, 4, file); little(file, size + 36, 4);
    fwrite("WAVEfmt ", 1, 8, file); little(file, 16, 4);
    little(file, 1, 2); little(file, 1, 2); little(file, AUDIO_RATE, 4);
    little(file, AUDIO_RATE * 2, 4); little(file, 2, 2); little(file, 16, 2);
    fwrite("data", 1, 4, file); little(file, size, 4);
    static face_t face;
    static tess_sound_t sound;
    face_init(&face); face_set_character(&face, CHARACTER_TESS);
    face_set_mode(&face, MODE_IDLE);
    for (int i = 0; i < 120; i++) face_update(&face, 1.f / 30);
    face_set_mode(&face, MODE_OFFLINE);
    tess_sound_reset(&sound); tess_sound_seed(&sound, 37);
    unsigned contacts = 0;
    int peak = 0;
    for (int frame = 0; frame < FRAMES; frame++) {
        float t = frame / 30.f;
        // Settle, roll twice, then stop. Same motion for every sound revision.
        float angle = t < 3 ? 0 : t < 11 ? sinf((t - 3) * .9f) * 2.2f : 0;
        face.grav_x = sinf(angle); face.grav_y = cosf(angle);
        face_update(&face, 1.f / 30);
        tess_cue_t cue; float strength, identity;
        while (face_take_cue(&face, &cue, &strength, &identity)) {
            if (cue != TC_IMPACT) continue;
            tess_sound_cue(&sound, cue, strength, identity); contacts++;
        }
        int32_t pcm[SAMPLES] = {0};
        tess_sound_mix(&sound, pcm, SAMPLES);
        for (int i = 0; i < SAMPLES; i++) {
            int magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (magnitude > peak) peak = magnitude;
            little(file, (uint16_t)(int16_t)pcm[i], 2);
        }
    }
    int error = ferror(file); fclose(file);
    printf("%u physical contact cues, peak %d/32768, 16 s preview\n", contacts, peak);
    return error ? 1 : 0;
}
