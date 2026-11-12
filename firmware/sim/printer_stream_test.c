#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "printer_stream.h"

static const char rid[] = "0123456789abcdef0123456789abcdef";
static void frame(uint8_t *out, unsigned offset, unsigned count, uint8_t fill) {
    memcpy(out, rid, 32); out[32] = offset >> 8; out[33] = offset; out[34] = count;
    memset(out + PRINTER_STREAM_FRAME_HEADER, fill, count * PRINTER_STREAM_ROW_BYTES);
}
static printer_stream_t *start(unsigned rows) {
    printer_stream_t *s = printer_stream_create(rid, 7, rows); assert(s);
    printer_stream_credit_t credit;
    assert(!printer_stream_credit(s, &credit));
    printer_stream_ready(s);
    assert(printer_stream_credit(s, &credit) && !strcmp(credit.rid, rid) && credit.session == 7 && credit.offset == 0);
    assert(!printer_stream_credit(s, &credit));
    return s;
}
static void correct_order(void) {
    printer_stream_t *s = start(18); uint8_t data[PRINTER_STREAM_FRAME_HEADER + 16 * PRINTER_STREAM_ROW_BYTES], row[72];
    frame(data, 0, 16, 0xa5);
    printer_stream_receive(s, 8, data, sizeof data); // Stale route cannot write.
    assert(printer_stream_row(s, row) == PRINTER_STREAM_WAIT);
    data[0] = 'f'; printer_stream_receive(s, 7, data, sizeof data); data[0] = '0';
    assert(printer_stream_row(s, row) == PRINTER_STREAM_WAIT);
    printer_stream_receive(s, 7, data, sizeof data);
    printer_stream_credit_t credit;
    for (unsigned i = 0; i < 16; ++i) {
        assert(printer_stream_row(s, row) == PRINTER_STREAM_ROW && row[0] == 0xa5 && row[71] == 0xa5);
        assert(!printer_stream_credit(s, &credit)); printer_stream_consumed(s);
    }
    assert(printer_stream_credit(s, &credit) && credit.offset == 16);
    frame(data, 16, 2, 0x5a); printer_stream_receive(s, 7, data, PRINTER_STREAM_FRAME_HEADER + 2 * 72);
    for (unsigned i = 0; i < 2; ++i) {
        assert(printer_stream_row(s, row) == PRINTER_STREAM_ROW && row[0] == 0x5a); printer_stream_consumed(s);
    }
    assert(!printer_stream_credit(s, &credit));
    assert(printer_stream_row(s, row) == PRINTER_STREAM_FAILED);
    printer_stream_destroy(s);
}
static void invalid_frame(unsigned offset, unsigned count, size_t length) {
    printer_stream_t *s = start(16); uint8_t data[PRINTER_STREAM_FRAME_HEADER + 16 * 72] = {0}, row[72];
    frame(data, offset, count <= 16 ? count : 0, 0); data[34] = count;
    printer_stream_receive(s, 7, data, length);
    assert(printer_stream_failed(s) && printer_stream_row(s, row) == PRINTER_STREAM_FAILED);
    printer_stream_destroy(s);
}
static void failures(void) {
    invalid_frame(1, 1, PRINTER_STREAM_FRAME_HEADER + 72);
    invalid_frame(0, 0, PRINTER_STREAM_FRAME_HEADER);
    invalid_frame(0, 17, PRINTER_STREAM_FRAME_HEADER);
    invalid_frame(0, 1, PRINTER_STREAM_FRAME_HEADER + 71);
    printer_stream_t *s = start(1); uint8_t data[PRINTER_STREAM_FRAME_HEADER + 72], row[72];
    frame(data, 0, 1, 0xff); printer_stream_receive(s, 7, data, sizeof data);
    printer_stream_receive(s, 7, data, sizeof data); // duplicate while slot is full
    assert(printer_stream_failed(s) && printer_stream_row(s, row) == PRINTER_STREAM_FAILED);
    printer_stream_destroy(s);
    s = start(1); printer_stream_abort(s); assert(printer_stream_failed(s));
    printer_stream_destroy(s);
}
int main(void) { correct_order(); failures(); puts("printer image stream: bounded credit, session, ordering, duplicate and abort passed"); }
