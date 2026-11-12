#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#define PRINTER_PREVIEW_KIND 0x07
#define PRINTER_PREVIEW_VERSION 1
#define PRINTER_PREVIEW_BYTES 8224
#define PRINTER_PREVIEW_CHUNK 896
#if CONFIG_IDF_TARGET_ESP32S3
#define PRINTER_PREVIEW_EDGE 128
#else
#define PRINTER_PREVIEW_EDGE 64
#endif
#define PRINTER_PREVIEW_STORAGE (32 + PRINTER_PREVIEW_EDGE * PRINTER_PREVIEW_EDGE / 2)

typedef struct printer_preview printer_preview_t;
void printer_preview_receive(uint8_t tag, const uint8_t *data, size_t length, uint32_t session);
bool printer_preview_activate(uint32_t session, const char *rid);
void printer_preview_discard(uint32_t session, const char *rid);
void printer_preview_clear(uint32_t session);
printer_preview_t *printer_preview_snapshot(uint32_t session);
void printer_preview_retain(printer_preview_t *preview);
void printer_preview_release(printer_preview_t *preview);
const uint8_t *printer_preview_pixels(const printer_preview_t *preview);
uint32_t printer_preview_hash(const printer_preview_t *preview);
void printer_preview_presented(printer_preview_t *preview);
bool printer_preview_was_presented(const printer_preview_t *preview);
