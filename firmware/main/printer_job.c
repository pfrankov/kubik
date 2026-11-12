#include "printer_job.h"
#include "printer_result.h"
#include "printer_guard.h"
#include "printer_platform.h"
#include "printer_status_internal.h"
#include "peripage_ble.h"
#include "ble_runtime.h"
#include "link.h"
#include <string.h>

#define TEAR_OFF_MM 10

static bool current(const printer_job_t *job) {
    return !atomic_load(&job->cancelled) && job->session == link_session() &&
        (!job->image || !printer_stream_failed(job->stream)) &&
        printer_platform_now_ms() < job->deadline_ms;
}

static const char *open_error(peripage_result_t result) {
    switch (result) {
    case PERIPAGE_NOT_FOUND: return "printer_not_found";
    case PERIPAGE_AMBIGUOUS: return "printer_ambiguous";
    case PERIPAGE_UNSUPPORTED: return "printer_unsupported";
    case PERIPAGE_BUSY: return "printer_busy";
    default: return "printer_failed";
    }
}

static bool packet(printer_job_t *job, const uint8_t *data, size_t bytes) {
    if (!current(job)) return false;
    int64_t remaining = job->deadline_ms - printer_platform_now_ms();
    if (remaining <= 0) return false;
    uint32_t timeout = remaining < 3000 ? (uint32_t)remaining : 3000;
    if (!timeout) return false;
    job->dispatched = true; // Even a failed first write can have reached the printer.
    return peripage_write_packet(data, bytes, timeout);
}

static bool chunk_begin(printer_job_t *job, unsigned rows) {
    static const uint8_t reset[16] = {0x10,0xff,0xfe,1};
    // Confirmed A6+ concentration 0..2: use 2 for solid black text and QR.
    static const uint8_t density[] = {0x10,0xff,0x10,0,2};
    uint8_t header[] = {0x1d,0x76,0x30,0,72,0,(uint8_t)rows,0};
    return packet(job, reset, sizeof reset) && packet(job, density, sizeof density) &&
        packet(job, header, sizeof header);
}

static bool image_row(printer_job_t *job, uint8_t row[PRINTER_STREAM_ROW_BYTES]) {
    while (current(job)) {
        printer_stream_read_t state = printer_stream_row(job->stream, row);
        if (state == PRINTER_STREAM_ROW) return true;
        if (state == PRINTER_STREAM_FAILED) return false;
        printer_platform_wait_tick();
    }
    return false;
}

static bool print_rows(printer_job_t *job, printer_layout_t *layout,
                       const printer_layout_info_t *info, unsigned total) {
    uint8_t row[72];
    for (unsigned y = 0; y < total; y++) {
        memset(row, 0, sizeof row);
        if (y < info->height_dots && !(job->image ? image_row(job, row) :
            printer_layout_row(layout, y, row, sizeof row))) return false;
        if (y % 255 == 0 && !chunk_begin(job, total - y > 255 ? 255 : total - y)) return false;
        if (!packet(job, row, sizeof row)) return false;
        if (job->image && y < info->height_dots) printer_stream_consumed(job->stream);
    }
    return true;
}

static bool finish_stream(printer_job_t *job, peripage_info_t *printer) {
    if (!current(job)) return false;
    int64_t remaining = job->deadline_ms - printer_platform_now_ms();
    if (remaining <= 0) return false;
    return peripage_finish_packet_stream(remaining < 3000 ? (uint32_t)remaining : 3000, printer);
}

static const char *send_document(printer_job_t *job, printer_layout_t *layout,
                                 const printer_layout_info_t *info, peripage_info_t *printer) {
    unsigned trailing_rows = (TEAR_OFF_MM * 10u * info->feed_dpi + 253u) / 254u;
    unsigned total = info->height_dots + trailing_rows;
    size_t packets = total + 3 * ((total + 254) / 255);
    int64_t remaining = job->deadline_ms - printer_platform_now_ms();
    if (!current(job) || remaining < peripage_packet_stream_budget_ms(packets)) return "printer_busy";
    if (job->image) printer_stream_ready(job->stream);
    return print_rows(job, layout, info, total) && finish_stream(job, printer) ? NULL : "printer_disconnected";
}

static uint32_t open_timeout(const printer_job_t *job) {
    int64_t remaining = job->deadline_ms - printer_platform_now_ms();
    if (remaining < 4000) return 0;
    return remaining < 12000 ? (uint32_t)remaining : 12000;
}

static const char *print_document(printer_job_t *job, peripage_info_t *info,
                                 printer_layout_info_t *layout_info, bool *have_layout) {
    printer_layout_error_t problem;
    printer_layout_t *layout = printer_layout_create(&job->document, info->dpi, info->feed_dpi, info->width_dots, &problem);
    if (!layout) return printer_layout_error_code(problem);
    *layout_info = *printer_layout_info(layout); *have_layout = true;
    const char *error = send_document(job, layout, layout_info, info);
    printer_layout_destroy(layout);
    return error;
}

static const char *print_image(printer_job_t *job, peripage_info_t *info,
                               printer_layout_info_t *layout, bool *have_layout) {
    unsigned maximum = (unsigned)(PRINTER_LAYOUT_MAX_HEIGHT_MM * info->feed_dpi / 25.4);
    if (info->width_dots != PRINTER_STREAM_ROW_BYTES * 8 || !job->image_width ||
        job->image_width > info->width_dots || !job->image_height || job->image_height > maximum)
        return "content_does_not_fit";
    *layout = (printer_layout_info_t){.format=PRINTER_FORMAT_IMAGE, .width_dots=job->image_width,
        .height_dots=job->image_height, .head_width_dots=info->width_dots, .dpi=info->dpi,
        .feed_dpi=info->feed_dpi, .width_mm=job->image_width * 25.4 / info->dpi,
        .height_mm=job->image_height * 25.4 / info->feed_dpi};
    *have_layout = true;
    return send_document(job, NULL, layout, info);
}

static void observe_job(const printer_job_t *job, const peripage_info_t *info, bool identified,
                        bool closed, const char *error) {
    if (atomic_load(&job->cancelled) || job->session != link_session()) return;
    bool verified = identified && closed && (!job->dispatched || !error);
    printer_status_observe(identified ? info : NULL, verified, verified ? NULL : error);
}

char *printer_job_run(printer_job_t *job, bool *found) {
    *found = false;
    peripage_info_t info = {0};
    printer_layout_info_t layout_info = {0};
    bool have_layout = false;
    const char *error = "printer_disconnected";
    uint32_t timeout = open_timeout(job);
    if (!current(job) || !timeout) return job->automatic ? NULL : printer_result_make(job, error, NULL, NULL);
    printer_guard_begin();
    peripage_result_t opened = peripage_open(&info, timeout);
    bool identified = opened == PERIPAGE_OK;
    *found = identified;
    error = opened == PERIPAGE_OK ? NULL : open_error(opened);
    if (*found && job->printing) error = job->image ? print_image(job, &info, &layout_info, &have_layout) :
        print_document(job, &info, &layout_info, &have_layout);
    bool closed = peripage_close();
    if (ble_runtime_owned(BLE_RUNTIME_PRINTER)) { printer_platform_radio_fault(); return NULL; }
    printer_guard_released();
    if (!closed) { error = "printer_failed"; *found = false; }
    observe_job(job, &info, identified, closed, error);
    if (job->automatic) return NULL;
    return printer_result_make(job, error, identified ? &info : NULL, have_layout ? &layout_info : NULL);
}
