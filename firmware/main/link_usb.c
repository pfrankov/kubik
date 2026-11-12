#include "task_placement.h"
#include "link_usb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link.h"

_Static_assert(LINK_USB_CONFIG_REPLY_CAP > LINK_USB_PAYLOAD_MAX, "reply includes input sentinel");
static SemaphoreHandle_t s_usb_mtx;
static link_usb_handlers_t s_handlers;
typedef struct {
    uint8_t *frame;
    uint8_t type, crc;
    bool no_memory;
    int state;
    size_t len, received, capacity;
    int64_t deadline;
} usb_parser_t;
#define USB_FRAME_TIMEOUT_US 5000000

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
        // IDF enqueues each call atomically; an item larger than its TX ring
        // can never succeed. Stream bounded chunks under the whole-frame lock.
        size_t chunk = len < 1024 ? len : 1024;
        int written = usb_serial_jtag_write_bytes(data, chunk, timeout);
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

static void wipe_bytes(void *data, size_t length) {
    volatile uint8_t *p = data;
    while (length--) *p++ = 0;
}

#define STATUS_CHUNK_BYTES 384
typedef struct status_chunk {
    struct status_chunk *next;
    size_t used;
    uint8_t bytes[STATUS_CHUNK_BYTES];
} status_chunk_t;
typedef struct { status_chunk_t *head, *tail; size_t length; } status_chunks_t;
static bool status_chunk_write(void *context, char value) {
    status_chunks_t *chunks = context;
    if (!chunks->tail || chunks->tail->used == STATUS_CHUNK_BYTES) {
        status_chunk_t *chunk = heap_caps_calloc(1, sizeof *chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!chunk) return false;
        if (chunks->tail) chunks->tail->next = chunk;
        else chunks->head = chunk;
        chunks->tail = chunk;
    }
    chunks->tail->bytes[chunks->tail->used++] = (uint8_t)value;
    chunks->length++;
    return true;
}
static void status_chunks_free(status_chunks_t *chunks) {
    while (chunks->head) {
        status_chunk_t *chunk = chunks->head;
        chunks->head = chunk->next;
        wipe_bytes(chunk, sizeof *chunk);
        heap_caps_free(chunk);
    }
}
static bool status_chunks_send(const status_chunks_t *chunks) {
    uint8_t header[5] = {0xa5, 0x5a, F_CONFIG_REPLY, (uint8_t)chunks->length, (uint8_t)(chunks->length >> 8)};
    uint8_t crc = 0;
    for (status_chunk_t *chunk = chunks->head; chunk; chunk = chunk->next)
        crc = link_usb_crc8(crc, chunk->bytes, chunk->used);
    if (xSemaphoreTake(s_usb_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    TickType_t timeout = pdMS_TO_TICKS(30);
    bool ok = usb_write_all(header, sizeof header, timeout);
    for (status_chunk_t *chunk = chunks->head; ok && chunk; chunk = chunk->next)
        ok = usb_write_all(chunk->bytes, chunk->used, timeout);
    if (ok) ok = usb_write_all(&crc, 1, timeout);
    xSemaphoreGive(s_usb_mtx);
    return ok;
}
static void parser_release(usb_parser_t *parser);
static bool dispatch_info(void *parsed) {
    if (!s_handlers.on_config_info) return false;
    status_chunks_t chunks = {0};
    bool ok = false;
    bool handled = s_handlers.on_config_info(parsed, status_chunk_write, &chunks, &ok);
    if (handled) {
        if (ok) status_chunks_send(&chunks);
        else {
            const char error[] = "{\"ok\":false,\"error\":\"status unavailable\"}";
            link_usb_frame(F_CONFIG_REPLY, error, sizeof error - 1, NULL, 0);
        }
    }
    status_chunks_free(&chunks);
    return handled;
}
static void dispatch_config(usb_parser_t *parser) {
    if (!s_handlers.on_config_parse || !s_handlers.on_config_reply || !s_handlers.on_config_free) return;
    void *parsed = s_handlers.on_config_parse((char *)parser->frame, parser->len);
    // cJSON owns copies of the request strings; raw credentials no longer occupy the RX buffer.
    parser_release(parser);
    if (!dispatch_info(parsed)) {
        char *reply = heap_caps_malloc(LINK_USB_CONFIG_REPLY_CAP,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (reply) {
            s_handlers.on_config_reply(parsed, reply, LINK_USB_CONFIG_REPLY_CAP);
            reply[LINK_USB_CONFIG_REPLY_CAP - 1] = 0;
            if (reply[0]) link_usb_frame(F_CONFIG_REPLY, reply, strlen(reply), NULL, 0);
            wipe_bytes(reply, LINK_USB_CONFIG_REPLY_CAP);
            heap_caps_free(reply);
        } else {
            const char error[] = "{\"ok\":false,\"error\":\"USB buffer unavailable\"}";
            link_usb_frame(F_CONFIG_REPLY, error, sizeof error - 1, NULL, 0);
        }
    }
    s_handlers.on_config_free(parsed);
}
static void dispatch_frame(usb_parser_t *parser) {
    uint8_t type = parser->type, *frame = parser->frame;
    size_t len = parser->len;
    frame[len] = 0;
    if (type == F_HOST_HELLO) {
        if (s_handlers.on_host_hello) s_handlers.on_host_hello((char *)frame, len);
    } else if (type == F_CONFIG) {
        dispatch_config(parser);
    } else if (type == F_JSON || type == F_AUDIO || type == F_BIND) {
        if (s_handlers.on_routed) s_handlers.on_routed(type, frame, len);
    }
}

static void parser_release(usb_parser_t *parser) {
    if (parser->frame) {
        wipe_bytes(parser->frame, parser->capacity);
        heap_caps_free(parser->frame);
    }
    parser->frame = NULL; parser->capacity = 0; parser->deadline = 0;
}

static void parser_reset(usb_parser_t *parser) {
    parser_release(parser);
    memset(parser, 0, sizeof *parser);
}

static void parser_error(usb_parser_t *parser, const char *message) {
    if (parser->type == F_CONFIG) link_usb_frame(F_CONFIG_REPLY, message, strlen(message), NULL, 0);
}

static void parser_expire(usb_parser_t *parser) {
    if (!parser->frame || esp_timer_get_time() < parser->deadline) return;
    parser_release(parser);
    // Preserve the declared boundary: late payload bytes are never new headers.
    parser_error(parser, "{\"ok\":false,\"error\":\"USB frame timeout\"}");
}

static TickType_t parser_wait(const usb_parser_t *parser) {
    int64_t ms = parser->frame ? (parser->deadline - esp_timer_get_time() + 999) / 1000 : 1000;
    if (ms > 1000) ms = 1000;
    TickType_t ticks = ms > 0 ? pdMS_TO_TICKS(ms) : 0;
    return ticks ? ticks : 1;
}

static void parser_begin(usb_parser_t *parser) {
    bool config = parser->type == F_CONFIG;
    bool json = parser->type == F_HOST_HELLO;
    bool routed = parser->type == F_JSON || parser->type == F_AUDIO || parser->type == F_BIND;
    size_t limit = config || json ? LINK_JSON_MAX : LINK_USB_PAYLOAD_MAX;
    parser->received = 0; parser->crc = 0;
    parser->state = parser->len ? 5 : 6;
    if ((!config && !json && !routed) || parser->len > limit) return;
    parser->capacity = parser->len + 1;
    parser->frame = heap_caps_malloc(parser->capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    parser->no_memory = !parser->frame;
    if (parser->frame) parser->deadline = esp_timer_get_time() + USB_FRAME_TIMEOUT_US;
}

static void parser_header_byte(usb_parser_t *parser, uint8_t byte) {
    switch (parser->state) {
    case 0: parser->state = byte == 0xA5 ? 1 : 0; break;
    case 1: parser->state = byte == 0x5A ? 2 : (byte == 0xA5 ? 1 : 0); break;
    case 2: parser->type = byte; parser->state = 3; break;
    case 3: parser->len = byte; parser->state = 4; break;
    case 4:
        parser->len |= (size_t)byte << 8;
        parser_begin(parser);
        break;
    }
}

static bool parser_feed_byte(usb_parser_t *parser, uint8_t byte) {
    if (parser->state < 5) {
        parser_header_byte(parser, byte);
        return false;
    }
    if (parser->state == 5) {
        if (parser->frame) parser->frame[parser->received] = byte;
        else if (parser->no_memory) parser->crc = link_usb_crc8(parser->crc, &byte, 1);
        parser->received++;
        if (parser->received == parser->len) parser->state = 6;
        return false;
    }
    if (parser->frame && link_usb_crc8(0, parser->frame, parser->len) == byte) return true;
    if (parser->no_memory && parser->crc == byte)
        parser_error(parser, "{\"ok\":false,\"error\":\"USB buffer unavailable\"}");
    parser_reset(parser);
    return false;
}

static void usb_rx_task(void *arg) {
    (void)arg;
    static usb_parser_t parser;
    uint8_t chunk[256];
    while (1) {
        parser_expire(&parser);
        int count = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), parser_wait(&parser));
        parser_expire(&parser);
        for (int i = 0; i < count; i++) {
            if (!parser_feed_byte(&parser, chunk[i])) continue;
            dispatch_frame(&parser);
            parser_reset(&parser);
        }
    }
}

void link_usb_start(const link_usb_handlers_t *handlers) {
    s_handlers = *handlers;
    s_usb_mtx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_usb_mtx ? ESP_OK : ESP_ERR_NO_MEM);
    // Finish lazy table setup before the RX task and application senders can race.
    link_usb_crc8(0, NULL, 0);
    usb_serial_jtag_driver_config_t cfg = {.rx_buffer_size = 4096, .tx_buffer_size = 2048};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(usb_rx_task, "usb_rx", 3584, NULL, 8, NULL, KUBIK_UI_CORE) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
