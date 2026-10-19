// CST9217/CST9220 report layout verified against Waveshare 1.0.4 and SensorLib CST92xx.
// Two points are supported by the verified driver path; no guessed extra registers.
#pragma once

#include <stddef.h>
#include "touch_input.h"

#define CST9217_REPORT_BYTES (TOUCH_POINTS * 5 + 5)
typedef struct { touch_contact_t point[TOUCH_POINTS]; uint8_t count; } touch_report_t;

static inline bool touch_report_decode(const uint8_t *data, size_t size, touch_report_t *out) {
    *out = (touch_report_t){0};
    if (size != CST9217_REPORT_BYTES || data[6] != 0xab) return false;
    unsigned count = data[5] & 0x7f;
    if (count > TOUCH_POINTS) return false;
    if (!count || data[0] == 0xab || (data[4] & 0x80)) return true;
    touch_report_t frame = {0};
    uint16_t seen = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *p = data + i * 5 + (i ? 2 : 0);
        unsigned id = p[0] >> 4, status = p[0] & 15;
        if (status == 0) continue; // lift; a later record may still hold another finger
        if (status != 6 || (seen & (1u << id))) return false;
        seen |= 1u << id;
        frame.point[frame.count++] = (touch_contact_t){
            .x = (uint16_t)((p[1] << 4) | (p[3] >> 4)),
            .y = (uint16_t)((p[2] << 4) | (p[3] & 15)), .id = id,
        };
    }
    *out = frame;
    return true;
}
