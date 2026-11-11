#include "render_internal.h"

#include <string.h>

void render_frame(render_state_t *st, const scene_t *cs, render_workspace_t *workspace, bool swap,
                  render_push_fn push, render_ready_fn ready, void *ctx) {
    scene_t *s = (scene_t *)cs;
    st->prep_us = render_raster_prepare(s, swap);
    render_raster_window(s, workspace->band, 0, R_W - 1, 0, R_BAND - 1);
    push(0, 0, R_W, R_BAND, workspace->band, ctx);
    for (int y = R_BAND; y < R_H; y += R_BAND) {
        render_raster_window(s, workspace->lookahead, 0, R_W - 1, y, y);
        push(0, y, R_W, y + 1, workspace->lookahead, ctx);
        if (ready) ready(y - R_BAND, workspace->band, ctx);
        memcpy(workspace->band, workspace->lookahead, sizeof workspace->lookahead);
        render_raster_window(s, workspace->band + R_W, 0, R_W - 1, y + 1, y + R_BAND - 1);
        push(0, y + 1, R_W, y + R_BAND, workspace->band + R_W, ctx);
    }
    if (ready) ready(R_H - R_BAND, workspace->band, ctx);
    st->pixels_last = R_W * R_H;
    render_raster_get_timings(st);
}
