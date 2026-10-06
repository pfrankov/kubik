// IMA ADPCM speech codec: bit-exact with the server's encoder (openclaw-kubik test/protocol.test.js checks the
// same hash), speech-grade SNR, and a stream decoded in arbitrary pieces equals one decoded at once.
// cc -O2 -I../main ima_adpcm_test.c -lm -o out/ima_adpcm_test && out/ima_adpcm_test
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ima_adpcm.c"
#include "link_mic.c"

#define N 24000
static int16_t src[N], whole[N], pieces[N];
static uint8_t packet[IMA_HEADER_BYTES + N / 2];
static uint8_t *enc = packet + IMA_HEADER_BYTES;

static void check_mic_frames(void) {
    size_t encoded = 0;
    uint8_t reference[483]; ima_state_t mic = {0};
    const uint8_t *frame = link_mic_encode(7, 21, src, 1920, NULL, &encoded);
    ima_encode(&mic, src, 960, reference);
    assert(encoded == 485 && frame[0] == 4 && frame[1] == 7 && !memcmp(frame + 2, reference, 483));
    frame = link_mic_encode(7, 21, src + 960, 1920, NULL, &encoded);
    ima_encode(&mic, src + 960, 960, reference);
    assert(!memcmp(frame + 2, reference, 483));
    frame = link_mic_encode(8, 21, src, 1920, NULL, &encoded);
    assert(frame[2] == 0 && frame[3] == 0 && frame[4] == 0);
    frame = link_mic_encode(8, 22, src, 1920, NULL, &encoded);
    assert(frame[2] == 0 && frame[3] == 0 && frame[4] == 0);
    assert(!link_mic_encode(8, 22, src, 1924, NULL, &encoded));
    assert(!link_mic_encode(8, 22, src, 1919, NULL, &encoded));
    assert(!link_mic_encode(8, 22, NULL, 1920, NULL, &encoded));
}

static void write_mic_vector(const char *path) {
    FILE *out = fopen(path, "wb"); assert(out);
    for (int i = 0; i < 4; i++) {
        size_t bytes; int16_t decoded[960]; ima_state_t state;
        const uint8_t *frame = link_mic_encode(i < 2 ? 9 : 10, i == 3 ? 32 : 31, src + 960 * i, 1920, NULL, &bytes);
        assert(bytes == 485 && fwrite(frame, 1, bytes, out) == bytes);
        assert(ima_read_header(frame + 2, &state));
        ima_decode(&state, frame + 5, 480, decoded);
        assert(fwrite(decoded, 2, 960, out) == 960);
    }
    assert(!fclose(out));
}

int main(int argc, char **argv) {
    for (int i = 0; i < N; i++) src[i] = (int16_t)lround(8000 * sin(i * 0.07) + 3000 * sin(i * 0.31));
    ima_state_t e = {0};
    unsigned hash = 2166136261u;
    ima_encode(&e, src, N, packet);
    for (int i = 0; i < N / 2; i++) hash = (hash ^ enc[i]) * 16777619u;
    assert(hash == 0x14f87442u);
    ima_state_t d = {0};
    ima_decode(&d, enc, N / 2, whole);
    ima_state_t p = {0};
    for (int off = 0, len = 1; off < N / 2; off += len, len = len % 240 + 7) {
        if (off + len > N / 2) len = N / 2 - off;
        ima_decode(&p, enc + off, len, pieces + off * 2);
    }
    double sig = 0, err = 0;
    for (int i = 0; i < N; i++) {
        assert(whole[i] == pieces[i]);
        sig += (double)src[i] * src[i];
        err += (double)(src[i] - whole[i]) * (src[i] - whole[i]);
    }
    double snr = 10 * log10(sig / err);
    uint8_t hdr[3] = {0x34, 0x12, 89};
    assert(!ima_read_header(hdr, &p));
    hdr[2] = 40;
    assert(ima_read_header(hdr, &p) && p.pred == 0x1234 && p.index == 40);
    check_mic_frames();
    if (argc == 2) write_mic_vector(argv[1]);
    printf("IMA ADPCM: server-exact, SNR %.1f dB, piecewise decode identical\n", snr);
    assert(snr > 30);
}
