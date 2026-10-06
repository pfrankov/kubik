#include "present.h"

#include <string.h>

#include "esp_timer.h"

// The stripes of a rectangle changed on the native rows y0..y1-1 it covers.
static void mask_union(uint16_t *mask, const canvas_rect_t *r, int y0, int y1) {
    if (r->x0 >= r->x1) return;
    uint16_t stripes = (uint16_t)(((1u << ((r->x1 - 1) / CANVAS_STRIPE + 1)) - 1) & ~((1u << (r->x0 / CANVAS_STRIPE)) - 1));
    for (int y = r->y0 > y0 ? r->y0 : y0; y < r->y1 && y < y1; y++) mask[y] |= stripes;
}

// The diff only sends what changed, so a pixel the panel once took wrongly would stay forever.
#define HEAL_ROWS 2  // native rows per frame: 1920 panel px, the whole screen every 4 s
#define HEAL_BUSY_PX 70000
// The store's room a band of nothing but noise needs: 9 rows of 15 stripes, 64 bytes each, at worst.
#define FLUSH_BYTES (FRAME_STORE_BYTES - (R_BAND + 1) * FRAME_STRIPES * FRAME_STRIPE * 4)

// Sends one panel window: the native rows it covers are expanded 2x and the edge band and text drawn over them.
static void send_window(frame_out_t *f, const render_win_t *w) {
    int64_t t = esp_timer_get_time();
    if (!f->burst_start) f->burst_start = t;
    int width = w->x1 - w->x0;
    render_output_columns(&f->output, w->x0, w->x1);
    display_begin(w->x0, w->y0, w->x1, w->y1);
    int chunk = (LCD_W * OUTPUT_ROWS / width) & ~1;  // a narrow window fills the same buffer with more rows: fewer DMA transfers
    for (int y = w->y0; y < w->y1; y += chunk) {
        int rows = w->y1 - y < chunk ? w->y1 - y : chunk;
        uint16_t *out = f->bufs[f->next];
        f->next = (f->next + 1) % LCD_PIPE;
        render_expand_2x(&f->output, out, w->x0, y, width, rows, true);
        render_edge_overlay(f->scene, out, w->x0, y, width, rows, true);
        render_text_overlay(f->scene, out, w->x0, y, width, rows, true);
        display_write(out, width * rows, y + rows == w->y1);
    }
    display_end();
    f->expand_us += esp_timer_get_time() - t;
    f->px += width * (w->y1 - w->y0);
    f->windows++;
}

static int larger(int a, int b) { return a > b ? a : b; }

// How far in from each side the edge band reaches on the rows of band y0, counting what the panel still shows of it
// (the larger of the two covers both), or -1 when the edge band there looks the same as before.
static int edge_reach(const frame_out_t *f, int y0) {
    int band = y0 / R_BAND;
    if (f->edge_sig == f->band_edge_sig[band]) return -1;
    int reach = render_edge_reach(larger(f->scene->edge_w, f->band_edge_w[band]), larger(f->scene->edge_r, f->band_edge_r[band]),
                                  y0 * R_SCALE, (y0 + R_BAND) * R_SCALE);
    return reach + (reach & 1);
}

// Whether a window already holds the whole band (the edge band's sides with it).
static bool holds_band(const render_win_t *w, int y0) {
    return w->x0 == 0 && w->x1 == LCD_W && w->y0 <= y0 * R_SCALE && w->y1 >= (y0 + R_BAND) * R_SCALE;
}

// A window that continues one of the band above with the same columns is sent as one: each window costs the panel
// bus commands. The first of a chain of them is extended, the others are marked to be skipped.
static void join_windows(frame_out_t *f, int band, int bottom) {
    render_win_t *w = f->plan[band], *open[PLAN_WINDOWS];
    int opened = 0;
    f->plan_skip[band] = 0;
    for (int i = 0; i < f->plan_n[band]; i++) {
        render_win_t *root = NULL;
        for (int j = 0; j < f->open_n && !root; j++) {
            render_win_t *o = f->open[j];
            if (o->x0 == w[i].x0 && o->x1 == w[i].x1 && o->y1 == w[i].y0) root = o;
        }
        if (root) {
            root->y1 = w[i].y1;
            f->plan_skip[band] |= (uint16_t)(1u << i);
        }
        if (w[i].y1 == bottom) open[opened++] = root ? root : &w[i];
    }
    memcpy(f->open, open, (size_t)opened * sizeof *open);
    f->open_n = opened;
}

// Adds a window (rounded out to whole pixel pairs) to the n planned, unless one of them already holds it.
static int add_window(render_win_t *w, int n, int x0, int y0, int x1, int y1) {
    x0 &= ~1; y0 &= ~1; x1 = (x1 + 1) & ~1; y1 = (y1 + 1) & ~1;
    for (int i = 0; i < n; i++)
        if (w[i].x0 <= x0 && w[i].x1 >= x1 && w[i].y0 <= y0 && w[i].y1 >= y1) return n;
    w[n] = (render_win_t){(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1};
    return n + 1;
}

// Whether the edge band of this band's rows only changed its width (the panel shows one of another width, or none:
// the ring of the wider one is all that differs), so that render_edge_change finds what to resend.
static bool edge_width_changed(const frame_out_t *f, int band) {
    int w0 = f->band_edge_w[band], w1 = f->scene->edge_w;
    if (!w0 || !w1) return true;
    return render_edge_radius(w0, f->band_edge_r[band]) == render_edge_radius(w1, f->scene->edge_r) &&
           !((f->band_edge_sig[band] ^ f->edge_sig) & 0xFFFF);  // the colour
}

// The windows of the pixels the edge band's new width colours differently, on the rows of band y0.
static int add_edge_ring(const frame_out_t *f, int y0, render_win_t *w, int n) {
    int band = y0 / R_BAND, top = y0 * R_SCALE, bottom = top + R_BAND * R_SCALE;
    int w1 = f->scene->edge_w, r = w1 ? f->scene->edge_r : f->band_edge_r[band];
    edge_change_t c = render_edge_change(f->band_edge_w[band], w1, r, top, bottom);
    if (c.x0 < c.x1) {
        n = add_window(w, n, c.x0, top, c.x1, bottom);
        n = add_window(w, n, LCD_W - c.x1, top, LCD_W - c.x0, bottom);
    }
    return c.sy0 < c.sy1 ? add_window(w, n, c.r, c.sy0, LCD_W - c.r, c.sy1) : n;
}

// The windows of the edge band's whole reach when its look changed in other ways: its sides, where the body's windows
// do not hold the band already.
static int add_edge_sides(render_win_t *w, int n, int y0, int reach) {
    bool held = false;
    for (int i = 0; i < n; i++) held |= holds_band(&w[i], y0);
    if (held) return n;
    w[n++] = (render_win_t){0, (int16_t)(y0 * R_SCALE), (int16_t)reach, (int16_t)((y0 + R_BAND) * R_SCALE)};
    w[n++] = (render_win_t){(int16_t)(LCD_W - reach), (int16_t)(y0 * R_SCALE), LCD_W, (int16_t)((y0 + R_BAND) * R_SCALE)};
    return n;
}

// The windows of band y0, from what changed in it and the edge band. An edge band that only grows or retracts (it
// does when listening starts and ends) is redrawn where its pixels change: a ring of a few pixels, not the band's
// whole reach, which would be most of the screen's border. Any other change of it (colour, radius) is redrawn where
// it can be: the columns on either side, or the whole band of rows when it reaches across.
static void plan_band(frame_out_t *f, int y0) {
    int band = y0 / R_BAND, reach = edge_reach(f, y0);
    render_win_t *w = f->plan[band];
    bool ring = reach > 0 && edge_width_changed(f, band);
    int n;
    if (reach > 0 && !ring && 2 * reach >= LCD_W) {
        w[0] = (render_win_t){0, (int16_t)(y0 * R_SCALE), LCD_W, (int16_t)((y0 + R_BAND) * R_SCALE)};
        n = 1;
    } else {
        n = render_band_windows(f->mask, y0, y0 + R_BAND, w);
        if (ring) n = add_edge_ring(f, y0, w, n);
        else if (reach > 0) n = add_edge_sides(w, n, y0, reach);
    }
    f->plan_n[band] = (uint8_t)n;
    f->plan_reach[band] = (int16_t)reach;
    join_windows(f, band, (y0 + R_BAND) * R_SCALE);
}

// Sends the windows planned for band y0 and remembers what the panel now shows of its edge band.
static void send_band(frame_out_t *f, int y0) {
    int band = y0 / R_BAND;
    for (int i = 0; i < f->plan_n[band]; i++)
        if (!(f->plan_skip[band] >> i & 1)) send_window(f, &f->plan[band][i]);
    if (f->plan_reach[band] < 0) return;
    f->band_edge_w[band] = f->scene->edge_w;
    f->band_edge_r[band] = f->scene->edge_r;
    f->band_edge_sig[band] = f->edge_sig;
}

// The stripes of the native rows y0..y0+R_BAND (the last one is the next band's first row) the windows of band y0
// read: their columns, one stripe to the right for the last column's neighbour, on the rows they cover and the row
// below the last (the odd panel line blends in the next one).
static void band_needs(const frame_out_t *f, int y0, uint16_t need[R_BAND + 1]) {
    memset(need, 0, (R_BAND + 1) * sizeof *need);
    int band = y0 / R_BAND;
    for (int i = 0; i < f->plan_n[band]; i++) {
        const render_win_t *w = &f->plan[band][i];
        int first = (w->x0 / R_SCALE) / CANVAS_STRIPE, last = (w->x1 / R_SCALE) / CANVAS_STRIPE;
        if (last >= FRAME_STRIPES) last = FRAME_STRIPES - 1;
        uint16_t stripes = (uint16_t)(((1u << (last + 1)) - 1) & ~((1u << first) - 1));
        for (int y = w->y0 / R_SCALE; y <= w->y1 / R_SCALE; y++) need[y - y0] |= stripes;
    }
}

// A frame too dense for the store (the worst case of the next band would not fit) is sent in two: the bands kept so
// far now (the row y0 that closes the last of them goes into the store first), the rest after the store is reused.
// A rare, brief tear across the split instead of a frame lost.
static void flush_store(frame_out_t *f, int y0, const uint16_t *rows) {
    frame_store_row(&f->store, y0, f->border_need, rows);
    render_output_begin(&f->output, frame_store_read, &f->store);
    for (int y = f->sent * R_BAND; y < y0; y += R_BAND) send_band(f, y);
    f->sent = y0 / R_BAND;
    f->border_need = 0;
    f->open_n = 0;
    if (f->store.used > f->store_max) f->store_max = f->store.used;
    frame_store_begin(&f->store);
    f->flushes++;
}

// Plans the burst for a finished band and keeps in the store what it reads (the band's buffer is still alive: the next
// band renders into the other one): a band nothing changed in costs nothing. The first row of a band is also its
// predecessor's interpolation border.
static void keep_band(frame_out_t *f, int y0) {
    const uint16_t *rows = f->bands[(y0 / R_BAND) & 1];
    uint16_t need[R_BAND + 1];
    if (f->store.used > FLUSH_BYTES && y0 > 0) flush_store(f, y0, rows);
    int64_t t = esp_timer_get_time();  // after the flush: what a band costs to keep, not the burst it may have sent
    plan_band(f, y0);
    band_needs(f, y0, need);
    need[0] |= f->border_need;
    for (int i = 0; i < R_BAND; i++)
        if (need[i]) frame_store_row(&f->store, y0 + i, need[i], rows + i * R_W);
    f->border_need = need[R_BAND];
    f->bands_kept++;
    f->pack_us += esp_timer_get_time() - t;
}

// Render callback: keep the band and find what changed in it, without touching the panel.
static void push(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx) {
    (void)x0; (void)x1;
    frame_out_t *f = ctx;
    int64_t t = esp_timer_get_time();
    canvas_rows(&f->canvas, y0, y1, px);
    memcpy(f->mask + y0, f->canvas.changed_mask + y0, (size_t)(y1 - y0) * sizeof *f->mask);
    mask_union(f->mask, &(canvas_rect_t){0, f->heal_y, CANVAS_W, f->heal_y + f->heal_h}, y0, y1);
    for (int y = y0; y < y1; y++) f->mask[y] |= f->overlay_mask[y];
    f->hash_us += esp_timer_get_time() - t;
    if (y0 > 0) keep_band(f, y0 - R_BAND);
}

static void overlay_mark(frame_out_t *f, const render_overlay_item_t *item) {
    canvas_rect_t r = {item->x0, item->y0, item->x1, item->y1};
    mask_union(f->overlay_mask, &r, 0, CANVAS_H);
}
static void overlay_prepare(frame_out_t *f, const scene_t *scene) {
    memset(f->overlay_mask, 0, sizeof f->overlay_mask);
    unsigned count = 0;
    for (int i = 0; i < scene->n; i++) {
        render_overlay_item_t item;
        if (!render_overlay_item(scene, &scene->p[i], &item)) continue;
        if (count >= f->overlay_count || memcmp(&item, &f->overlay_prev[count], sizeof item)) {
            if (count < f->overlay_count) overlay_mark(f, &f->overlay_prev[count]);
            overlay_mark(f, &item);
        }
        f->overlay_prev[count++] = item;
    }
    for (unsigned i = count; i < f->overlay_count; i++) overlay_mark(f, &f->overlay_prev[i]);
    f->overlay_count = count;
}
bool present_frame(frame_out_t *frame, render_state_t *state, scene_t *scene) {
    // Ordered item comparison also repaints overlapping overlays after reordering.
    overlay_prepare(frame, scene);
    frame->edge_sig = render_edge_signature(scene);
    // Only while the previous frame was light: in the heavy ones (recording) it would cost fps.
    frame->heal_y = (frame->heal_y + frame->heal_h) % CANVAS_H;
    frame->heal_h = frame->px - frame->heal_px0 < HEAL_BUSY_PX ? HEAL_ROWS : 0;
    frame->heal_px0 = frame->px;
    frame_store_begin(&frame->store);
    frame->border_need = 0;
    frame->open_n = 0;
    frame->bands_kept = 0;
    frame->sent = 0;
    render_frame(state, scene, frame->bands, 2, false, push, frame);
    keep_band(frame, R_H - R_BAND);
    if (!frame->store.valid) {  // it does not fit the store: nothing is sent, the next frame repaints the whole screen
        frame->overflows++;
        canvas_reset(&frame->canvas);
        return false;
    }
    if (frame->store.used > frame->store_max) frame->store_max = frame->store.used;
    frame->burst_start = 0;
    render_output_begin(&frame->output, frame_store_read, &frame->store);
    for (int y = frame->sent * R_BAND; y < R_H; y += R_BAND) send_band(frame, y);
    if (frame->burst_start) {
        int64_t gap = frame->shown_at ? frame->burst_start - frame->shown_at : 0;
        if (gap && (!frame->gap_min || gap < frame->gap_min)) frame->gap_min = gap;
        if (gap > frame->gap_max) frame->gap_max = gap;
        int64_t burst = esp_timer_get_time() - frame->burst_start;
        frame->burst_sum += burst;
        frame->bursts++;
        if (burst > frame->burst_max) frame->burst_max = burst;
    }
    frame->shown_at = frame->burst_start;  // 0 for a frame that changed nothing: no spacing is measured across it
    return true;
}
