#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// Design pixels preserve the same physical scale on each panel. Rendering uses
// whole eight-row bands; the S3 adapter clips the final padding below UI_H.
#define UI_W 480
#define UI_CX (UI_W / 2)
#if CONFIG_IDF_TARGET_ESP32S3
#define UI_H 584
#define UI_RENDER_H 592
#else
#define UI_H 480
#define UI_RENDER_H 480
#endif
#define UI_CY (UI_H / 2)
#define UI_FOOTER_Y (UI_H - 52)
