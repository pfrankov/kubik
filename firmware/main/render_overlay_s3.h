#pragma once
#include "render.h"

// Native S3 overlays retain design-space sources; no intermediate RGB image.
void render_s3_text_overlay(const scene_t *scene, uint16_t *out, int x0, int y0, int width, int rows, int stride, bool swap);
void render_s3_edge_overlay(const scene_t *scene, uint16_t *out, int x0, int y0, int width, int rows, int stride, bool swap);
int render_s3_edge_reach(int width, int radius, int y0, int y1);
edge_change_t render_s3_edge_change(int before, int after, int radius, int y0, int y1);
