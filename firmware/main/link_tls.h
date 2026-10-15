#pragma once
#include "esp_transport.h"
#include <stdbool.h>
// Link owns both handles until all senders and the SDK task have stopped.
esp_transport_handle_t link_tls_create(bool pinned);
esp_transport_handle_t link_tls_wss(const char *uri, bool pinned, esp_transport_handle_t *parent);
