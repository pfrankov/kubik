#include "display_s3_tx.h"
#include "viewport.h"
#include "board.h"
#include "task_placement.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <assert.h>
#include <string.h>

#define TX_BANDS (DISPLAY_S3_HEIGHT / DISPLAY_S3_ROWS)
#define TX_WAIT_US 40000
#define TX_STACK_BYTES 4096
#define TX_PRIORITY (configMAX_PRIORITIES - 2)
_Static_assert(TX_BANDS <= 64 && DISPLAY_S3_HEIGHT % DISPLAY_S3_ROWS == 0, "Native band credit mask");
// Share core0 with the SDK Wi-Fi task (23), whose startup can remain ready
// longer than the strict START deadline. Equal-priority tick slicing is required.
_Static_assert(configMAX_PRIORITIES == 25 && configUSE_TIME_SLICING == 1,
               "S3 sender requires the pinned SDK Wi-Fi peer priority and time slicing");
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static StaticTask_t s_task_storage;
static StackType_t s_stack[TX_STACK_BYTES / sizeof(StackType_t)];
static StaticSemaphore_t s_work_storage, s_start_storage, s_done_storage, s_credit_storage;
static SemaphoreHandle_t s_work, s_start, s_done, s_credit;
static uint16_t *s_image, *s_dma[3];
static int s_next;
static uint64_t s_pending;
static bool s_active, s_have_completed;
static int64_t s_started_us;
static uint32_t s_sequence;
static display_s3_profile_t s_completed;
static display_s3_tx_bursts_t s_bursts;
static struct {
    uint16_t left[TX_BANDS], right[TX_BANDS];
    display_s3_profile_t profile;
} s_job;

static TickType_t remaining_ticks(int64_t deadline) {
    int64_t remaining = deadline - esp_timer_get_time();
    ESP_ERROR_CHECK(remaining > 0 ? ESP_OK : ESP_ERR_TIMEOUT);
    TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
    return ticks ? ticks : 1;
}

void display_s3_tx_wait_idle(void) {
    if (!s_task) return; // Physical panel initialization precedes sender creation.
    int64_t deadline = esp_timer_get_time() + TX_WAIT_US;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        bool active = s_active;
        portEXIT_CRITICAL(&s_lock);
        if (!active) return;
        ESP_ERROR_CHECK(xSemaphoreTake(s_done, remaining_ticks(deadline)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
    }
}

void display_s3_tx_wait_writable(int y) {
    assert(s_task && y >= 0 && y < DISPLAY_S3_HEIGHT);
    uint64_t bit = UINT64_C(1) << (y / DISPLAY_S3_ROWS);
    int64_t deadline = esp_timer_get_time() + TX_WAIT_US;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        bool pending = (s_pending & bit) != 0;
        portEXIT_CRITICAL(&s_lock);
        if (!pending) return;
        ESP_ERROR_CHECK(xSemaphoreTake(s_credit, remaining_ticks(deadline)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
    }
}

static void IRAM_ATTR first_pixel(void *unused) {
    (void)unused;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_lock);
    assert(s_active && !s_started_us);
    s_started_us = now;
    portEXIT_CRITICAL_ISR(&s_lock);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_start, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void release_credit(int band) {
    portENTER_CRITICAL(&s_lock);
    s_pending &= ~(UINT64_C(1) << band);
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_credit);
}

static void stream_window(int first, int end, int x0, int x1) {
    s_job.profile.windows++;
    board_display_begin(x0, first * DISPLAY_S3_ROWS, x1, end * DISPLAY_S3_ROWS);
    int width = x1 - x0;
    for (int band = first; band < end; band++) {
        uint16_t *pixels = s_dma[s_next]; s_next = (s_next + 1) % 3;
        int y = band * DISPLAY_S3_ROWS;
        for (int row = 0; row < DISPLAY_S3_ROWS; row++)
            memcpy(pixels + row * width, s_image + (y + row) * UI_RENDER_W + x0, (size_t)width * 2);
        // This band is never read from the canonical image again by this job.
        release_credit(band);
        board_display_write(pixels, width * DISPLAY_S3_ROWS, band + 1 == end);
    }
    board_display_end();
}

static void complete_job(void) {
    board_display_wait_idle(); // Release this worker's bus only after its DMA tail.
    int64_t completed_at = esp_timer_get_time();
    int sender_core = xPortGetCoreID();
    uint32_t stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&s_lock);
    assert(s_started_us && !s_pending);
    s_job.profile.burst_us = completed_at - s_started_us;
    s_job.profile.sender_core = sender_core; s_job.profile.sender_stack_free = stack_free;
    s_completed = s_job.profile;
    s_have_completed = true; s_sequence++;
    s_bursts.sum_us += s_completed.burst_us;
    if (s_completed.burst_us > s_bursts.max_us) s_bursts.max_us = s_completed.burst_us;
    s_bursts.count++;
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_done);
}

static void sender(void *unused) {
    (void)unused;
    for (;;) {
        xSemaphoreTake(s_work, portMAX_DELAY);
        board_display_frame_start(first_pixel, NULL);
        for (int band = 0; band < TX_BANDS;) {
            int first = band++, x0 = s_job.left[first], x1 = s_job.right[first];
            if (x0 >= x1) continue;
            while (band < TX_BANDS && s_job.left[band] == x0 && s_job.right[band] == x1) band++;
            stream_window(first, band, x0, x1);
        }
        complete_job();
    }
}

static void delete_semaphores(void) {
    SemaphoreHandle_t *handles[] = {&s_work, &s_start, &s_done, &s_credit};
    for (unsigned i = 0; i < sizeof handles / sizeof handles[0]; i++) {
        if (*handles[i]) vSemaphoreDelete(*handles[i]);
        *handles[i] = NULL;
    }
}

static bool create_sender(void) {
    s_work = xSemaphoreCreateBinaryStatic(&s_work_storage);
    s_start = xSemaphoreCreateBinaryStatic(&s_start_storage);
    s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
    s_credit = xSemaphoreCreateBinaryStatic(&s_credit_storage);
    if (!s_work || !s_start || !s_done || !s_credit) { delete_semaphores(); return false; }
    s_task = xTaskCreateStaticPinnedToCore(sender, "s3_spi", TX_STACK_BYTES, NULL, TX_PRIORITY,
                                         s_stack, &s_task_storage, KUBIK_AUDIO_CORE);
    if (!s_task) { delete_semaphores(); return false; }
    return true;
}

bool display_s3_tx_init(uint16_t *image, uint16_t *dma[3]) {
    if (!image || !dma || !dma[0] || !dma[1] || !dma[2]) return false;
    display_s3_tx_wait_idle();
    if (!s_task && !create_sender()) return false;
    portENTER_CRITICAL(&s_lock);
    s_image = image;
    memcpy(s_dma, dma, sizeof s_dma);
    s_next = 0;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

int64_t display_s3_tx_submit(const uint16_t *left, const uint16_t *right, const display_s3_profile_t *profile) {
    assert(s_task && left && right && profile);
    xSemaphoreTake(s_start, 0);
    portENTER_CRITICAL(&s_lock);
    assert(!s_active && !s_pending);
    memcpy(s_job.left, left, sizeof s_job.left); memcpy(s_job.right, right, sizeof s_job.right);
    s_job.profile = *profile; s_job.profile.windows = 0; s_job.profile.burst_us = 0;
    for (int band = 0; band < TX_BANDS; band++) {
        assert(left[band] <= DISPLAY_S3_WIDTH && right[band] <= DISPLAY_S3_WIDTH);
        if (left[band] < right[band]) s_pending |= UINT64_C(1) << band;
    }
    assert(s_pending);
    s_started_us = 0; s_active = true;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_work);
    BaseType_t ready = xSemaphoreTake(s_start, pdMS_TO_TICKS(40));
    ESP_ERROR_CHECK(ready == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
    portENTER_CRITICAL(&s_lock);
    int64_t started = s_started_us;
    portEXIT_CRITICAL(&s_lock);
    assert(started);
    return started;
}

bool display_s3_tx_last_completed(display_s3_profile_t *profile, uint32_t *sequence) {
    assert(profile && sequence);
    portENTER_CRITICAL(&s_lock);
    bool ready = s_have_completed;
    if (ready) { *profile = s_completed; *sequence = s_sequence; }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

display_s3_tx_bursts_t display_s3_tx_take_bursts(void) {
    portENTER_CRITICAL(&s_lock);
    display_s3_tx_bursts_t result = s_bursts;
    memset(&s_bursts, 0, sizeof s_bursts);
    portEXIT_CRITICAL(&s_lock);
    return result;
}
