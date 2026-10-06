#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    F_JSON = 0x01,
    F_AUDIO = 0x02,
    F_LOG = 0x10,
    F_HOST_HELLO = 0x20,
    F_CONFIG = 0x21,
    F_CONFIG_REPLY = 0x22,
    F_BIND = 0x23,  // epoch + the transport binding the bridge saw upstream
};

typedef struct {
    void (*on_host_hello)(const char *json, size_t len);
    void (*on_config)(const char *json, size_t len, char *reply, size_t cap);
    void (*on_routed)(uint8_t type, uint8_t *data, size_t len);
} link_usb_handlers_t;

void link_usb_start(const link_usb_handlers_t *handlers);
uint8_t link_usb_crc8(uint8_t crc, const uint8_t *data, size_t len);
bool link_usb_frame(uint8_t type, const void *first, size_t first_len,
                    const void *second, size_t second_len);
// Log output: framed while a bridge listens, plain text for a serial monitor.
int link_usb_log_vprintf(const char *fmt, va_list ap);
