#include "mic_capture_task.h"

#include "mic_capture.h"
#include "ima_adpcm.h"
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

static bool copy_channels(const mic_task_config_t *cfg, int16_t *raw, int16_t *voice,
                          uint8_t *reference, int filled, ima_state_t *encoder, int32_t *dc) {
    bool duplex = cfg->duplex && cfg->duplex(cfg->context);
    for (int i = 0; i < cfg->chunk_samples; i++) voice[filled + i] = raw[cfg->slots * i + cfg->voice_slot];
    if (duplex) {
        // Reuse the DMA scratch after MIC1 was copied. Compacting forwards never
        // overwrites a future TDM slot, and keeps a whole extra PCM frame out of RAM.
        for (int i = 0; i < cfg->chunk_samples; i++) raw[i] = raw[cfg->slots * i + 1];
        mic_capture_center(raw, cfg->chunk_samples, dc);
        uint8_t ima[IMA_HEADER_BYTES + 240 / 2];
        ima_encode(encoder, raw, cfg->chunk_samples, ima);
        if (!filled) memcpy(reference, ima, IMA_HEADER_BYTES);
        memcpy(reference + IMA_HEADER_BYTES + filled / 2, ima + IMA_HEADER_BYTES, cfg->chunk_samples / 2);
    }
    return duplex;
}

static void read_failed(const mic_task_config_t *cfg, int32_t *dc, uint32_t epoch) {
    if (cfg->duplex && cfg->duplex(cfg->context) && cfg->requested(cfg->context))
        cfg->deliver(cfg->context, NULL, NULL, dc, epoch);
}

void mic_capture_task(void *arg) {
    const mic_task_config_t *cfg = arg;
    // CPU-only scratch is fully overwritten before delivery; preserve DMA RAM for Wi-Fi.
    static RTC_NOINIT_ATTR int16_t raw[4 * 240];
    static RTC_NOINIT_ATTR int16_t voice[960];
    static uint8_t reference[IMA_HEADER_BYTES + 960 / 2]; // Live only, synchronous codec loopback (TDM slot 1).
    int32_t dc = 0, reference_dc = 0;
    ima_state_t reference_encoder = {0};
    int filled = 0;
    int failed_starts = 0;
    uint32_t frame_epoch = 0;
    while (1) {
        bool requested = cfg->requested(cfg->context);
        if (!cfg->sync(cfg->context, requested)) {
            if (requested && ++failed_starts >= 3) {
                ESP_LOGE(TAG, "microphone start failed; waiting for PTT retry");
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            } else vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        failed_starts = 0;
        if (!requested) {
            filled = 0;
            dc = reference_dc = 0; reference_encoder = (ima_state_t){0};
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        uint32_t epoch = cfg->epoch(cfg->context);
        if (epoch != frame_epoch) { filled = 0; dc = reference_dc = 0; reference_encoder = (ima_state_t){0}; frame_epoch = epoch; }
        size_t bytes = cfg->slots * cfg->chunk_samples * sizeof(raw[0]);
        if (!cfg->read(cfg->context, true, raw, bytes, cfg->timeout_ms)) {
            read_failed(cfg, &dc, frame_epoch);
            vTaskDelay(1);
            continue;
        }
        if (!cfg->requested(cfg->context) || epoch != cfg->epoch(cfg->context)) { filled = 0; continue; }
        bool duplex = copy_channels(cfg, raw, voice, reference, filled, &reference_encoder, &reference_dc);
        filled += cfg->chunk_samples;
        if (filled == cfg->frame_samples) {
            cfg->deliver(cfg->context, voice, duplex ? reference : NULL, &dc, frame_epoch);
            filled = 0;
            // Bound DMA backlog draining; the single core must also run IDLE.
            vTaskDelay(1);
        }
    }
}
