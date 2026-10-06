#include "link_usb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link.h"

static SemaphoreHandle_t s_usb_mtx;
static link_usb_handlers_t s_handlers;
typedef struct {
    uint8_t frame[LINK_USB_RX_STORAGE];
    uint8_t type;
    int state;
    size_t len, received;
} usb_parser_t;

uint8_t link_usb_crc8(uint8_t crc, const uint8_t *data, size_t len) {
    static uint8_t table[256];
    static bool initialized;
    if (!initialized) {
        for (int value = 0; value < 256; value++) {
            uint8_t entry = (uint8_t)value;
            for (int bit = 0; bit < 8; bit++)
                entry = (entry & 0x80) ? (uint8_t)((entry << 1) ^ 0x07) : (uint8_t)(entry << 1);
            table[value] = entry;
        }
        initialized = true;
    }
    while (len--) crc = table[crc ^ *data++];
    return crc;
}

static bool usb_write_all(const uint8_t *data, size_t len, TickType_t timeout) {
    while (len) {
        int written = usb_serial_jtag_write_bytes(data, len, timeout);
        if (written <= 0) return false;
        data += written;
        len -= written;
    }
    return true;
}

bool link_usb_frame(uint8_t type, const void *first, size_t first_len,
                    const void *second, size_t second_len) {
    size_t len = first_len + second_len;
    if (len > 0xFFFF) return false;
    uint8_t header[5] = {0xA5, 0x5A, type, (uint8_t)len, (uint8_t)(len >> 8)};
    uint8_t crc = link_usb_crc8(0, first, first_len);
    crc = link_usb_crc8(crc, second, second_len);
    if (xSemaphoreTake(s_usb_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    TickType_t timeout = pdMS_TO_TICKS(30);
    bool ok = usb_write_all(header, sizeof(header), timeout) &&
              (!first_len || usb_write_all(first, first_len, timeout)) &&
              (!second_len || usb_write_all(second, second_len, timeout)) &&
              usb_write_all(&crc, 1, timeout);
    xSemaphoreGive(s_usb_mtx);
    return ok;
}

// Log output never waits: a full USB buffer (nobody reading) drops the line instead of stalling the
// caller, and a line is queued in one piece (the ring buffer takes an item whole or not at all), so
// it can neither tear a frame nor hold the mutex while the host is away.
static void usb_log_write(const void *data, size_t len) {
    if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || !s_usb_mtx) return;
    if (xSemaphoreTake(s_usb_mtx, 0) != pdTRUE) return;
    usb_serial_jtag_write_bytes(data, len, 0);
    xSemaphoreGive(s_usb_mtx);
}


int link_usb_log_vprintf(const char *fmt, va_list ap) {
    uint8_t out[5 + 256 + 1];
    char *text = (char *)out + 5;
    int n = vsnprintf(text, 256, fmt, ap);
    if (n <= 0) return n;
    if (n >= 256) n = 255;
    if (!link_usb_host_present()) {
        usb_log_write(text, n);
        return n;
    }
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    out[0] = 0xA5;
    out[1] = 0x5A;
    out[2] = F_LOG;
    out[3] = (uint8_t)n;
    out[4] = (uint8_t)(n >> 8);
    out[5 + n] = link_usb_crc8(0, (const uint8_t *)text, n);
    usb_log_write(out, 5 + n + 1);
    return n;
}

static void dispatch_frame(uint8_t type, uint8_t *frame, size_t len) {
    frame[len] = 0;
    if (type == F_HOST_HELLO) {
        if (s_handlers.on_host_hello) s_handlers.on_host_hello((char *)frame, len);
    } else if (type == F_CONFIG) {
        static char reply[2048];
        reply[0] = 0;
        if (s_handlers.on_config) s_handlers.on_config((char *)frame, len, reply, sizeof(reply));
        reply[sizeof(reply) - 1] = 0;
        if (reply[0]) link_usb_frame(F_CONFIG_REPLY, reply, strlen(reply), NULL, 0);
    } else if (type == F_JSON || type == F_AUDIO || type == F_BIND) {
        if (s_handlers.on_routed) s_handlers.on_routed(type, frame, len);
    }
}

static void parser_header_byte(usb_parser_t *parser, uint8_t byte) {
    switch (parser->state) {
    case 0: parser->state = byte == 0xA5 ? 1 : 0; break;
    case 1: parser->state = byte == 0x5A ? 2 : (byte == 0xA5 ? 1 : 0); break;
    case 2: parser->type = byte; parser->state = 3; break;
    case 3: parser->len = byte; parser->state = 4; break;
    case 4:
        parser->len |= (size_t)byte << 8;
        parser->received = 0;
        size_t limit = parser->type == F_CONFIG || parser->type == F_HOST_HELLO
                           ? LINK_JSON_MAX : LINK_USB_PAYLOAD_MAX;
        parser->state = parser->len > limit ? 0 : (parser->len ? 5 : 6);
        break;
    }
}

static bool parser_feed_byte(usb_parser_t *parser, uint8_t byte) {
    if (parser->state < 5) {
        parser_header_byte(parser, byte);
        return false;
    }
    if (parser->state == 5) {
        parser->frame[parser->received++] = byte;
        if (parser->received == parser->len) parser->state = 6;
        return false;
    }
    parser->state = 0;
    return link_usb_crc8(0, parser->frame, parser->len) == byte;
}

static void usb_rx_task(void *arg) {
    (void)arg;
    static usb_parser_t parser;
    uint8_t chunk[256];
    while (1) {
        int count = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(1000));
        for (int i = 0; i < count; i++)
            if (parser_feed_byte(&parser, chunk[i])) dispatch_frame(parser.type, parser.frame, parser.len);
    }
}

void link_usb_start(const link_usb_handlers_t *handlers) {
    s_handlers = *handlers;
    s_usb_mtx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_usb_mtx ? ESP_OK : ESP_ERR_NO_MEM);
    // Finish lazy table setup before the RX task and application senders can race.
    link_usb_crc8(0, NULL, 0);
    usb_serial_jtag_driver_config_t cfg = {.rx_buffer_size = 4096, .tx_buffer_size = 4096};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    ESP_ERROR_CHECK(xTaskCreate(usb_rx_task, "usb_rx", 3584, NULL, 8, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
