#include "printer_stream.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"

typedef enum { STARTING, NOTICE, WAITING, FULL, DONE, FAILED } stream_state_t;
struct printer_stream {
    char rid[33];
    uint32_t session;
    unsigned total, offset, count, used;
    stream_state_t state;
    uint8_t data[PRINTER_STREAM_ROWS * PRINTER_STREAM_ROW_BYTES];
};
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

printer_stream_t *printer_stream_create(const char *rid, uint32_t session, unsigned rows) {
    if (!rid || strlen(rid) != 32 || !session || !rows || rows > 3543) return NULL;
    printer_stream_t *stream = calloc(1, sizeof *stream);
    if (!stream) return NULL;
    memcpy(stream->rid, rid, 33); stream->session = session; stream->total = rows;
    return stream;
}
void printer_stream_destroy(printer_stream_t *stream) { free(stream); }
void printer_stream_ready(printer_stream_t *stream) {
    if (!stream) return;
    portENTER_CRITICAL(&lock);
    if (stream->state == STARTING) stream->state = NOTICE;
    portEXIT_CRITICAL(&lock);
}
bool printer_stream_credit(printer_stream_t *stream, printer_stream_credit_t *credit) {
    if (!stream || !credit) return false;
    portENTER_CRITICAL(&lock);
    bool available = stream->state == NOTICE;
    if (available) {
        memcpy(credit->rid, stream->rid, sizeof credit->rid);
        credit->session = stream->session; credit->offset = stream->offset;
        stream->state = WAITING;
    }
    portEXIT_CRITICAL(&lock);
    return available;
}
void printer_stream_abort(printer_stream_t *stream) {
    if (!stream) return;
    portENTER_CRITICAL(&lock); stream->state = FAILED; portEXIT_CRITICAL(&lock);
}
bool printer_stream_failed(printer_stream_t *stream) {
    if (!stream) return true;
    portENTER_CRITICAL(&lock);
    bool failed = stream->state == FAILED;
    portEXIT_CRITICAL(&lock);
    return failed;
}

void printer_stream_receive(printer_stream_t *stream, uint32_t session, const uint8_t *data, size_t length) {
    if (!stream || !data || length < 32 || session != stream->session || memcmp(data, stream->rid, 32)) return;
    portENTER_CRITICAL(&lock);
    if (length < PRINTER_STREAM_FRAME_HEADER || stream->state != WAITING) { stream->state = FAILED; goto done; }
    unsigned offset = ((unsigned)data[32] << 8) | data[33], count = data[34];
    if (offset != stream->offset || !count || count > PRINTER_STREAM_ROWS || count > stream->total - stream->offset ||
        length != PRINTER_STREAM_FRAME_HEADER + count * PRINTER_STREAM_ROW_BYTES) { stream->state = FAILED; goto done; }
    memcpy(stream->data, data + PRINTER_STREAM_FRAME_HEADER, count * PRINTER_STREAM_ROW_BYTES);
    stream->count = count; stream->used = 0; stream->state = FULL;
done:
    portEXIT_CRITICAL(&lock);
}

printer_stream_read_t printer_stream_row(printer_stream_t *stream, uint8_t row[PRINTER_STREAM_ROW_BYTES]) {
    if (!stream || !row) return PRINTER_STREAM_FAILED;
    portENTER_CRITICAL(&lock);
    printer_stream_read_t result = stream->state == FAILED || stream->state == DONE ? PRINTER_STREAM_FAILED :
        stream->state == FULL ? PRINTER_STREAM_ROW : PRINTER_STREAM_WAIT;
    if (result == PRINTER_STREAM_ROW)
        memcpy(row, stream->data + stream->used * PRINTER_STREAM_ROW_BYTES, PRINTER_STREAM_ROW_BYTES);
    portEXIT_CRITICAL(&lock);
    return result;
}
void printer_stream_consumed(printer_stream_t *stream) {
    if (!stream) return;
    portENTER_CRITICAL(&lock);
    if (stream->state == FULL && ++stream->used == stream->count) {
        stream->offset += stream->count;
        stream->state = stream->offset == stream->total ? DONE : NOTICE;
    }
    portEXIT_CRITICAL(&lock);
}
