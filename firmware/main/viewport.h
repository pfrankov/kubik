#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// Layout and touch use design pixels; output is native panel resolution.
// Storage is padded to whole 16-pixel canvas stripes, clipped before DMA.
#define UI_W 480
#define UI_CX (UI_W / 2)
#if CONFIG_IDF_TARGET_ESP32S3
#include "display_s3.h"
#define UI_H 584
#define UI_OUTPUT_W DISPLAY_S3_WIDTH
#define UI_OUTPUT_H DISPLAY_S3_HEIGHT
#define UI_RENDER_W 384
#define UI_RENDER_H 448
#define UI_SCALE_NUM 23
#define UI_SCALE_DEN 30
#else
#define UI_H 480
#define UI_OUTPUT_W 480
#define UI_OUTPUT_H 480
#define UI_RENDER_W 480
#define UI_RENDER_H 480
#define UI_SCALE_NUM 1
#define UI_SCALE_DEN 1
#endif
#define UI_CY (UI_H / 2)
#define UI_FOOTER_Y (UI_H - 52)

static inline int ui_output(int value) { return value * UI_SCALE_NUM / UI_SCALE_DEN; }
static inline int ui_canvas(int value) { return value * UI_SCALE_NUM / (UI_SCALE_DEN * 2); }
static inline float ui_canvasf(float value) { return value * UI_SCALE_NUM / (UI_SCALE_DEN * 2.f); }
