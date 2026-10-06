#include "render_internal.h"

void render_frame(render_state_t *st, const scene_t *cs, uint16_t **bufs, int nbufs, bool swap, render_push_fn push,
                  void *ctx) {
    scene_t *s = (scene_t *)cs;
    st->prep_us = render_raster_prepare(s, swap);
    for (int band = 0; band < R_BANDS; band++) {
        int y0 = band * R_BAND;
        uint16_t *buf = bufs[band % nbufs];
        render_raster_window(s, buf, 0, R_W - 1, y0, y0 + R_BAND - 1);
        push(0, y0, R_W, y0 + R_BAND, buf, ctx);
    }
    st->pixels_last = R_W * R_H;
    render_raster_get_timings(st);
}
