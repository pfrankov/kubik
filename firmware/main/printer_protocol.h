#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "status_json.h"

#define PRINTER_PROTOCOL_VERSION 2
#define PRINTER_IMAGE_VERSION 1
#define PRINTER_IMAGE_BINARY_KIND 0x06
#define PRINTER_TEXT_MAX_BYTES 1024
#define PRINTER_REPLY_SLOTS 4

// Authenticated RX holds the route mutex: only validate/copy one bounded job.
// A full outbox drops excess requests; the host permits one pending request/device.
bool printer_protocol_receive(cJSON *request, const char *raw, size_t len, size_t consumed, uint32_t session);
void printer_protocol_image_receive(uint8_t tag, const uint8_t *data, size_t len, uint32_t session);
// App task: sends at most one reply outside RX, without waking the screen.
// Stale sessions and failed sends are consumed without retry.
void printer_protocol_poll(void);
bool printer_protocol_busy(void);
// Last verified discovery in this route; not a fresh availability assertion.
bool printer_protocol_discovered(void);
bool printer_protocol_before_voice(void);
bool printer_protocol_reset_ready(void);
void printer_protocol_info(status_json_t *info);
