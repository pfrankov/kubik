#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define PRINTER_PROTOCOL_VERSION 1
#define PRINTER_TEXT_MAX_BYTES 1024
#define PRINTER_REPLY_SLOTS 4

// Authenticated RX holds the route mutex: never send or retain text here.
// A full outbox drops excess requests; the host permits one pending request/device.
bool printer_protocol_receive(cJSON *request, const char *raw, size_t len, size_t consumed, uint32_t session);
// App task: sends at most one reply outside RX, without waking the screen.
// Stale sessions and failed sends are consumed without retry.
void printer_protocol_poll(void);
