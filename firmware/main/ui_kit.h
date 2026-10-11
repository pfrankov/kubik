// Shared sub-page primitives: one header, one chip, one pager and one enter transition.
#pragma once
#include "render.h"

#define UI_ENTER_SECONDS 0.18f

// Truncate src to fit width, adding "..." when cut.
void ui_fit(char *out, size_t cap, const char *src, text_role_t role, float max_width);
// Colored dot + text in a capsule; align -1 left edge at x, 1 right edge at x.
void ui_chip(scene_t *s, const char *text, float x, float y, int align, uint32_t color);
// Large page title at the left; optional status chip at the right.
void ui_header(scene_t *s, const char *title, const char *chip, uint32_t chip_color);
// Equal segments across the bottom row; the current one is a gold capsule. Returns segment hit.
void ui_segments(scene_t *s, float y, const char *const *labels, unsigned count, unsigned current);
int ui_segment_hit(unsigned count, int x, int y, float row_y);
// Fade in every primitive added since `first` (index into s->p) over UI_ENTER_SECONDS.
void ui_enter(scene_t *s, int first, float elapsed);
