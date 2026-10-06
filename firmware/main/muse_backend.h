#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "link.h"

void muse_backend_init(const link_handlers_t *handlers, void (*route)(bool));
bool muse_backend_selected(void);
void muse_backend_allow(bool allowed);
bool muse_backend_wifi_stopped(void);
bool muse_backend_json(const char *json, uint32_t session);
bool muse_backend_pcm(uint8_t turn, const int16_t *pcm, size_t bytes, const uint8_t *ima, uint32_t session);
