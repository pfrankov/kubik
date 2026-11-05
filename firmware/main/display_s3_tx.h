#pragma once
#include "display_s3.h"

// One sender owns raw SPI. The UI may overwrite an8-row band only after its
// read credit is released; all DMA buffers are independent of the native image.
bool display_s3_tx_init(uint16_t *image, uint16_t *dma[3]);
void display_s3_tx_wait_writable(int y);
void display_s3_tx_wait_idle(void);
int64_t display_s3_tx_submit(const uint16_t *left, const uint16_t *right, const display_s3_profile_t *profile);
bool display_s3_tx_last_completed(display_s3_profile_t *profile, uint32_t *sequence);
typedef struct { int64_t sum_us, max_us; uint32_t count; } display_s3_tx_bursts_t;
display_s3_tx_bursts_t display_s3_tx_take_bursts(void);
