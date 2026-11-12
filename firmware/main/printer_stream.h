#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRINTER_STREAM_ROWS 16u
#define PRINTER_STREAM_ROW_BYTES 72u
#define PRINTER_STREAM_FRAME_HEADER 35u

typedef struct printer_stream printer_stream_t;
typedef enum { PRINTER_STREAM_WAIT, PRINTER_STREAM_ROW, PRINTER_STREAM_FAILED } printer_stream_read_t;
typedef struct { char rid[33]; uint32_t session; unsigned offset; } printer_stream_credit_t;

printer_stream_t *printer_stream_create(const char *rid, uint32_t session, unsigned rows);
void printer_stream_destroy(printer_stream_t *stream);
void printer_stream_ready(printer_stream_t *stream);
bool printer_stream_credit(printer_stream_t *stream, printer_stream_credit_t *credit);
void printer_stream_abort(printer_stream_t *stream);
bool printer_stream_failed(printer_stream_t *stream);
// Caller holds the job owner lock; stale IDs and sessions never affect the active stream.
void printer_stream_receive(printer_stream_t *stream, uint32_t session, const uint8_t *data, size_t length);
printer_stream_read_t printer_stream_row(printer_stream_t *stream, uint8_t row[PRINTER_STREAM_ROW_BYTES]);
void printer_stream_consumed(printer_stream_t *stream);
