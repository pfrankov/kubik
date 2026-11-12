#pragma once
#include "render.h"
void render_printer_preview(const scene_t *scene, const prim_t *prim, uint16_t *buffer,
                            int x0, int width, int y0, int y1, int band_y0);
