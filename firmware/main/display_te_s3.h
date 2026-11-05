// CO5300 vertical-porch synchronization on Waveshare S3 Touch-AMOLED-1.8 V2.
#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_te_init(void);
// Drain preceding pixels, discard a stale edge, then wait at most 40 ms.
// Call after preparing the complete native frame, immediately before its burst.
esp_err_t display_te_wait(void);

// Timestamp of the fresh edge accepted by the last successful wait.
int64_t display_te_last_edge_us(void);
