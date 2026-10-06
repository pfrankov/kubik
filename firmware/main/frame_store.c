#include "frame_store.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HOT IRAM_ATTR
#else
#define HOT
#endif

// A row keeps only the stripes asked for (the panel windows read no others), each run of neighbouring stripes as
// packets of pixel pairs (32-bit words, the unit the CPU moves and compares in one instruction):
// a header word, then either `count` literal words or (bit 31 set) one word repeated `count` times.
// Runs start at three equal words; everything else is a literal span.
#define ROW_WORDS (R_W / 2)
#define STRIPE_WORDS (FRAME_STRIPE / 2)
#define RUN_FLAG 0x80000000u

void frame_store_begin(frame_store_t *f) { f->used = 0; f->valid = true; }

static HOT unsigned encode_range(const uint32_t *w, int count, uint32_t *out) {
    const uint32_t *p = w, *end = w + count, *triple_stop = end - 2;  // a run of three needs two more words after p
    uint32_t *o = out;
    while (p < end) {
        const uint32_t *next = p + 1;
        while (next < end && *next == *p) next++;
        if (next - p >= 3) {
            *o++ = RUN_FLAG | (uint32_t)(next - p);
            *o++ = *p;
            p = next;
            continue;
        }
        uint32_t *head = o++;  // the literal span: its length is known once the words are copied
        *o++ = *p++;
        while (p < triple_stop) {
            if (p[1] != p[2]) {  // no run of three starts at p or at p + 1
                o[0] = p[0];
                o[1] = p[1];
                o += 2;
                p += 2;
            } else if (p[0] == p[1]) {
                break;
            } else {
                *o++ = *p++;
            }
        }
        if (p >= triple_stop) {
            while (p < end) *o++ = *p++;
        }
        *head = (uint32_t)(o - head - 1);
    }
    return (unsigned)(o - out);
}

// The next run of set stripes at or after k: its end (exclusive) is returned, or 0 when there is none.
static int next_run(uint32_t stripes, int *k) {
    while (*k < FRAME_STRIPES && !(stripes >> *k & 1)) (*k)++;
    if (*k >= FRAME_STRIPES) return 0;
    int end = *k;
    while (end < FRAME_STRIPES && (stripes >> end & 1)) end++;
    return end;
}

bool HOT frame_store_row(frame_store_t *f, int y, uint32_t stripes, const uint16_t *pixels) {
    if (!f->valid || y < 0 || y >= R_H) return false;
    uint32_t packed[ROW_WORDS * 2];  // worst case: a run and a literal alternate
    // The row goes straight into the store when even its worst case fits; near the end it is packed aside first.
    uint32_t *out = f->used + sizeof packed <= sizeof f->data ? f->data + f->used / 4 : packed;
    unsigned words = 0;
    const uint32_t *w = (const uint32_t *)(const void *)pixels;
    int k = 0;
    for (int end; (end = next_run(stripes, &k)); k = end)
        words += encode_range(w + k * STRIPE_WORDS, (end - k) * STRIPE_WORDS, out + words);
    if (f->used + words * 4 > sizeof f->data) { f->valid = false; return false; }
    f->row[y] = f->used / 4;
    f->stripes[y] = (uint16_t)stripes;
    if (out == packed) memcpy(f->data + f->used / 4, packed, words * 4);
    f->used += words * 4;
    return true;
}

// The words [lo, hi) of a run of `count` words (its packets start at p): copied to out, the other packets only skipped.
// Returns where the next run starts when fully traversed; the final requested run may stop early.
static HOT const uint32_t *decode_range(const uint32_t *p, uint32_t *out, int count, int lo, int hi) {
    for (int pos = 0; pos < count && pos < hi;) {
        uint32_t head = *p++;
        int n = (int)(head & 0xFFFF), from = pos > lo ? pos : lo, to = pos + n < hi ? pos + n : hi;
        if (head & RUN_FLAG) {
            uint32_t pair = *p++;
            for (int i = from; i < to; i++) out[i] = pair;
        } else {
            if (from < to) memcpy(out + from, p + (from - pos), (size_t)(to - from) * 4);
            p += n;
        }
        pos += n;
    }
    return p;
}

void HOT frame_store_read(void *ctx, int y, uint16_t *out, int xa, int xb) {
    const frame_store_t *f = ctx;
    const uint32_t *p = f->data + f->row[y];
    int lo = xa / 2, hi = (xb + 1) / 2;  // in words (pixel pairs) from the start of the row
    int k = 0;
    for (int end; (end = next_run(f->stripes[y], &k)); k = end) {
        int first = k * STRIPE_WORDS;
        if (first >= hi) break;
        p = decode_range(p, (uint32_t *)(void *)out + first, (end - k) * STRIPE_WORDS, lo - first, hi - first);
        if (end * STRIPE_WORDS >= hi) break;
    }
}
