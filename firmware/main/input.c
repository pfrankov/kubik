#include "app_lab.h"
#include "viewpoint.h"
#include "input.h"
#include "input_hold.h"

#include <math.h>
#include <stdlib.h>

#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_rub.h"
static const char *TAG = "input";

// ------------------------------------------------------------------ QMI8658
static i2c_master_dev_handle_t s_imu;
static bool imu_wr(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_imu, b, 2, 50) == ESP_OK;
}
static bool imu_rd(uint8_t reg, uint8_t *out, size_t n) {
    return i2c_master_transmit_receive(s_imu, &reg, 1, out, n, 50) == ESP_OK;
}
static bool imu_init(void) {
    const uint8_t addrs[2] = {0x6B, 0x6A};
    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i], .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(g_i2c_bus, &dc, &s_imu) != ESP_OK) continue;
        uint8_t who = 0;
        if (imu_rd(0x00, &who, 1) && who == 0x05) {
            imu_wr(0x60, 0xB0);  // soft reset
            vTaskDelay(pdMS_TO_TICKS(20));
            imu_wr(0x02, 0x40);  // CTRL1: address auto-increment, little endian
            imu_wr(0x03, 0x06);  // CTRL2: accel +-2 g, ~125 Hz
            imu_wr(0x04, 0x56);  // CTRL3: gyro +-512 dps, ~112 Hz
            imu_wr(0x08, 0x03);  // CTRL7: accelerometer and gyroscope on
            ESP_LOGI(TAG, "QMI8658 at 0x%02x", addrs[i]);
            return true;
        }
        i2c_master_bus_rm_device(s_imu);
        s_imu = NULL;
    }
    ESP_LOGW(TAG, "IMU not found");
    return false;
}

// Acceleration in g and rotation rate in rad/s (64 LSB per deg/s at +-512 dps).
static bool imu_read(float a[3], float w[3]) {
    uint8_t b[12];
    if (!s_imu || !imu_rd(0x35, b, 12)) return false;
    for (int i = 0; i < 3; i++) {
        a[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8)) / 16384.f;
        w[i] = (int16_t)(b[6 + 2 * i] | (b[7 + 2 * i] << 8)) * (3.14159265f / 180.f / 64.f);
    }
    return true;
}

// ------------------------------------------------------------------ state
#define BURST_MS 600  // dark: full-rate sampling after a press or touch
#define PICKUP_COS 0.82f  // cos 35 deg: how far from its resting pose counts as "picked up"
#define SHAKE_G 1.4f  // jolt (g, gravity removed) that counts towards a dizzying shake
static float s_tilt_x, s_tilt_y, s_view_x, s_view_y;
static float s_view_q[4] = {1, 0, 0, 0};
// Jolts accumulate as integers (1e-4 g*s) so the display can take differences without a lock.
static volatile int32_t s_jolt_x, s_jolt_y;
static float s_slide_x, s_slide_y, s_grav_x, s_grav_y;
// Gravity's pull along the screen (x right, y down) is minus what the accelerometer reads (it measures the
// reaction), and the sensor's axes are taken as the screen's. +1 keeps that; -1 turns an axis round. The offline
// fall of Tess's points came out upside down on the device with +1, +1, so both are turned. If the board ever
// changes, this is the one place to turn (the log line "gravity" shows the raw reading and the pull).
#define IMU_PULL_X_SIGN (-1)
#define IMU_PULL_Y_SIGN (-1)
static volatile bool s_ptt;
static volatile bool s_view_active;
void input_view_active(bool active) { s_view_active = active; }

void input_slide(float *x, float *y) {
    *x = s_slide_x;
    *y = s_slide_y;
}

void input_gravity(float *x, float *y) {
    *x = s_grav_x;
    *y = s_grav_y;
}

void input_tilt(float *x, float *y) {
    *x = s_tilt_x;
    *y = s_tilt_y;
}

void input_view_tilt(float *x, float *y) {
    *x = s_view_x;
    *y = s_view_y;
}
void input_view_quat(float q[4]) {
    for (int i = 0; i < 4; i++) q[i] = s_view_q[i];
}

void input_jolt(float *dvx, float *dvy) {
    static int32_t lx, ly;
    int32_t x = s_jolt_x, y = s_jolt_y;
    *dvx = (int32_t)(x - lx) * 1e-4f;
    *dvy = (int32_t)(y - ly) * 1e-4f;
    lx = x;
    ly = y;
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static volatile bool s_menu_view;  // the app's menu state, pushed on each change
void input_set_menu(bool on) { s_menu_view = on; }

typedef struct {
    int key_count, boot_count;
    bool key, boot, boot_long_sent;
    int64_t boot_started_ms;
    bool touching, gesture_done, first_drag;
    int drag_x, drag_y, touch_x0, touch_y0, touch_last_x, touch_last_y;
    float touch_path;
    int64_t touch_started_ms, touch_activity_ms;
    int missed_samples;
    bool imu;
    float gravity[3], rest[3], gyro_bias[3];
    bool gravity_initialized;
    int64_t last_motion_log_ms;
    bool rest_valid, rested;
    viewpoint_t viewpoint;
    int64_t last_view_us;
    int still_ms, shake_peaks, calm_ms, rest_length_ms;
    int64_t stirred_ms;
    int turned_ms;
    int64_t shake_started_ms, last_peak_ms, last_shake_ms, last_pickup_ms;
    int tick;
    // Sampling: every 20 ms while the screen is lit, every 40 ms while it is dark (see input_task).
    int sample_ms, sample_every, pmic_every;
    int64_t busy_until_ms;  // dark: full-rate sampling until here after a press or touch
} input_state_t;

typedef struct {
    float acceleration[3], rotation_rate[3];
    float dynamic, rotation;
    bool still;
} motion_sample_t;

static void update_ptt_key(input_state_t *s) {
    bool pressed = gpio_get_level(PIN_KEY) == 0;
    if (pressed != s->key) {
        if (++s->key_count >= 2) {
            s->key = pressed;
            s->key_count = 0;
            s_ptt = s->key;
            app_post(s->key ? EV_PTT_DOWN : EV_PTT_UP, 0, 0);
        }
    } else s->key_count = 0;
}

static void update_boot_key(input_state_t *s, int64_t now_ms) {
    bool pressed = gpio_get_level(PIN_BOOT) == 0;
    if (pressed != s->boot) {
        if (++s->boot_count >= 3) {
            s->boot = pressed;
            s->boot_count = 0;
            if (s->boot) {
                s->boot_started_ms = now_ms;
                s->boot_long_sent = false;
            } else if (!s->boot_long_sent) {
                app_post(EV_BOOT_SHORT, 0, 0);
            }
        }
    } else s->boot_count = 0;
    if (s->boot && !s->boot_long_sent && now_ms - s->boot_started_ms > 800) {
        s->boot_long_sent = true;
        app_post(EV_BOOT_LONG, 0, 0);
    }
}

static void update_power_key(const input_state_t *s) {
    if (s->tick % s->pmic_every != 0) return;
    int keys = pmic_pwr_keys();
    if (keys & PMIC_KEY_LONG) app_post(EV_PWR_LONG, 0, 0);
    else if (keys & PMIC_KEY_SHORT) app_post(EV_PWR_SHORT, 0, 0);
}

static void start_touch(input_state_t *s, int x, int y, int64_t now_ms) {
    app_post(EV_TOUCH_DOWN, 0, 0);
    s->touching = true;
    s->first_drag = true;
    s->gesture_done = false;
    s->touch_x0 = s->touch_last_x = x;
    s->touch_y0 = s->touch_last_y = y;
    s->touch_path = 0;
    s->touch_started_ms = s->touch_activity_ms = now_ms;
}

static void update_touch_path(input_state_t *s, int x, int y, int64_t now_ms) {
    if (!s->touching) {
        start_touch(s, x, y, now_ms);
        return;
    }
    s->touch_path += hypotf(x - s->touch_last_x, y - s->touch_last_y);
    s->touch_last_x = x;
    s->touch_last_y = y;
}

static void update_menu_drag(input_state_t *s, int x, int y, bool menu) {
    if (menu && (s->first_drag || abs(x - s->drag_x) + abs(y - s->drag_y) >= 4)) {
        app_post(EV_DRAG, x | (s->first_drag ? 1 << 16 : 0), y);
        s->drag_x = x;
        s->drag_y = y;
        s->first_drag = false;
    }
}

static void update_touch_gesture(input_state_t *s, int x, int y, int64_t now_ms, bool menu) {
    int dx = x - s->touch_x0, dy = y - s->touch_y0;
    float moved = hypotf(dx, dy);
    update_menu_drag(s, x, y, menu);
    if (input_hold_ready(menu, s->gesture_done, now_ms - s->touch_started_ms, s->touch_path)) {
        s->gesture_done = true;
        app_post(EV_MENU_HOLD, s->touch_x0, s->touch_y0);
    }
    if (!s->gesture_done && menu && -dy > 110 && abs(dx) < -dy && now_ms - s->touch_started_ms < 800) {
        s->gesture_done = true;
        app_post(EV_SWIPE_UP, 0, 0);
    }
    if (!menu && !s->gesture_done &&
        ((now_ms - s->touch_started_ms > 650 && moved < 30) || s->touch_path > 220)) {
        s->gesture_done = true;
        app_post(EV_PET, x, y);
    }
}

static void release_touch(input_state_t *s, int64_t now_ms) {
    s->touching = false;
    if (!s->gesture_done && now_ms - s->touch_started_ms < 450 && s->touch_path < 40) {
        app_post(EV_TAP, s->touch_x0, s->touch_y0);
    }
}

static void refresh_touch_activity(input_state_t *s, int64_t now_ms) {
    if (now_ms - s->touch_activity_ms < 200) return;
    app_post(EV_TOUCH_DOWN, 0, 0);
    s->touch_activity_ms = now_ms;
}

static void update_touch(input_state_t *s, int64_t now_ms) {
    if (s->tick % s->sample_every != 0) return;
    int x, y;
    bool has_touch = touch_read(&x, &y);
    if (has_touch) {
        s->missed_samples = 0;
        s->busy_until_ms = now_ms + BURST_MS;
        update_touch_path(s, x, y, now_ms);
        refresh_touch_activity(s, now_ms);
        update_touch_gesture(s, x, y, now_ms, s_menu_view);
        input_rub_sample(!s_menu_view || app_lab_active(), x, y);
    } else if (s->touching && ++s->missed_samples >= 3) {
        release_touch(s, now_ms);
        input_rub_sample(false, 0, 0);
    }
}

static void initialize_gravity(input_state_t *s, const float acceleration[3]) {
    if (s->gravity_initialized) return;
    for (int i = 0; i < 3; i++) s->gravity[i] = acceleration[i];
    s->gravity_initialized = true;
}

static float update_gravity(input_state_t *s, motion_sample_t *sample) {
    initialize_gravity(s, sample->acceleration);
    for (int i = 0; i < 3; i++) sample->rotation_rate[i] -= s->gyro_bias[i];
    const float dt = s->sample_ms * 0.001f;
    float predicted[3] = {
        s->gravity[0] - (sample->rotation_rate[1] * s->gravity[2] - sample->rotation_rate[2] * s->gravity[1]) * dt,
        s->gravity[1] - (sample->rotation_rate[2] * s->gravity[0] - sample->rotation_rate[0] * s->gravity[2]) * dt,
        s->gravity[2] - (sample->rotation_rate[0] * s->gravity[1] - sample->rotation_rate[1] * s->gravity[0]) * dt,
    };
    float dynamic_squared = 0;
    for (int i = 0; i < 3; i++) {
        float difference = sample->acceleration[i] - predicted[i];
        dynamic_squared += difference * difference;
        s->gravity[i] = predicted[i] + difference * (s->sample_ms == 20 ? 0.04f : 0.0784f);
    }
    return sqrtf(dynamic_squared);
}

static bool read_motion_sample(input_state_t *s, motion_sample_t *sample) {
    if (!imu_read(sample->acceleration, sample->rotation_rate)) return false;
    sample->dynamic = update_gravity(s, sample);
    float gravity_norm = sqrtf(s->gravity[0] * s->gravity[0] + s->gravity[1] * s->gravity[1] + s->gravity[2] * s->gravity[2]);
    if (gravity_norm > .2f) {
        s_grav_x = -IMU_PULL_X_SIGN * s->gravity[0] / gravity_norm;
        s_grav_y = -IMU_PULL_Y_SIGN * s->gravity[1] / gravity_norm;
    }
    float wx = sample->rotation_rate[0], wy = sample->rotation_rate[1], wz = sample->rotation_rate[2];
    sample->rotation = sqrtf(wx * wx + wy * wy + wz * wz);
    s_jolt_x += (int32_t)lrintf((sample->acceleration[0] - s->gravity[0]) * 200.f);
    s_jolt_y += (int32_t)lrintf((sample->acceleration[1] - s->gravity[1]) * 200.f);
    sample->still = sample->dynamic < 0.04f && sample->rotation < 0.15f;
    return true;
}

static void update_viewpoint(input_state_t *s, const motion_sample_t *sample) {
    if (s_view_active) {
        int64_t sample_us = esp_timer_get_time();
        float dt = s->last_view_us ? (sample_us - s->last_view_us) * 1e-6f : .02f;
        s->last_view_us = sample_us;
        viewpoint_step(&s->viewpoint, sample->rotation_rate,
                       sample->dynamic < .035f && sample->rotation < .04f, dt);
        s_view_x = s->viewpoint.x;
        s_view_y = s->viewpoint.y;
        for (int i = 0; i < 4; i++) s_view_q[i] = s->viewpoint.p[i];
    } else if (s->last_view_us) {
        viewpoint_init(&s->viewpoint);
        s->last_view_us = 0;
        s_view_x = s_view_y = 0;
        s_view_q[0] = 1;
        s_view_q[1] = s_view_q[2] = s_view_q[3] = 0;
    }
}

static void update_rest_timing(input_state_t *s, const motion_sample_t *sample) {
    s->still_ms = sample->still ? s->still_ms + s->sample_ms : 0;
    bool calm = sample->dynamic < 0.09f && sample->rotation < 0.35f;
    s->calm_ms = calm ? (s->calm_ms < 3600000 ? s->calm_ms + s->sample_ms : s->calm_ms) : 0;
    if (s->calm_ms > 0) s->rest_length_ms = s->calm_ms;
}

static void update_rest_pose(input_state_t *s) {
    if (!s->rest_valid && s->still_ms > 400) {
        for (int i = 0; i < 3; i++) s->rest[i] = s->gravity[i];
        s->rest_valid = true;
    }
    if (s->still_ms > 4000) {
        for (int i = 0; i < 3; i++) s->rest[i] += (s->gravity[i] - s->rest[i]) * 0.0002f * s->sample_ms;
    }
}

static void update_rest_state(input_state_t *s, const motion_sample_t *sample) {
    if (sample->still) for (int i = 0; i < 3; i++) s->gyro_bias[i] += sample->rotation_rate[i] * 0.001f * s->sample_ms;
    update_rest_timing(s, sample);
    update_rest_pose(s);
    if (s->rest_valid) {
        s_tilt_x = clampf((s->gravity[0] - s->rest[0]) * 2.2f, -1, 1);
        s_tilt_y = clampf((s->gravity[1] - s->rest[1]) * 2.2f, -1, 1);
        s_slide_x = s->rest[0] - s->gravity[0];
        s_slide_y = s->rest[1] - s->gravity[1];
    }
}

static void log_motion(input_state_t *s, const motion_sample_t *sample, int64_t now_ms) {
    static int64_t last_gravity_log_ms;
    if (now_ms - last_gravity_log_ms > 5000) {
        last_gravity_log_ms = now_ms;
        ESP_LOGI(TAG, "gravity raw %.2f,%.2f,%.2f pull on the screen %.2f,%.2f", s->gravity[0], s->gravity[1], s->gravity[2], s_grav_x, s_grav_y);
    }
    if ((sample->dynamic > 0.08f || sample->rotation > 0.6f) && now_ms - s->last_motion_log_ms > 250) {
        s->last_motion_log_ms = now_ms;
        ESP_LOGI(TAG, "motion lin %.2f,%.2f,%.2f rot %.1f,%.1f,%.1f slide %.2f,%.2f",
                 sample->acceleration[0] - s->gravity[0], sample->acceleration[1] - s->gravity[1],
                 sample->acceleration[2] - s->gravity[2], sample->rotation_rate[0], sample->rotation_rate[1],
                 sample->rotation_rate[2], s_slide_x, s_slide_y);
    }
}

static void detect_shake(input_state_t *s, const motion_sample_t *sample, int64_t now_ms) {
    if (!(sample->dynamic > SHAKE_G && now_ms - s->last_peak_ms > 120)) return;
    if (now_ms - s->shake_started_ms > 1200) {
        s->shake_started_ms = now_ms;
        s->shake_peaks = 0;
    }
    s->last_peak_ms = now_ms;
    ESP_LOGI(TAG, "hard jolt %.2f g (%d)", sample->dynamic, s->shake_peaks + 1);
    if (++s->shake_peaks >= 3 && now_ms - s->last_shake_ms > 3000) {
        s->last_shake_ms = now_ms;
        app_post(EV_SHAKE, 0, 0);
    }
}

static float rest_alignment(const input_state_t *s) {
    float gravity_norm = sqrtf(s->gravity[0] * s->gravity[0] + s->gravity[1] * s->gravity[1] + s->gravity[2] * s->gravity[2]);
    float rest_norm = sqrtf(s->rest[0] * s->rest[0] + s->rest[1] * s->rest[1] + s->rest[2] * s->rest[2]);
    if (!(s->rest_valid && gravity_norm > 0.3f && rest_norm > 0.3f)) return 1.f;
    return (s->gravity[0] * s->rest[0] + s->gravity[1] * s->rest[1] + s->gravity[2] * s->rest[2]) /
           (gravity_norm * rest_norm);
}

static void detect_pickup(input_state_t *s, const motion_sample_t *sample, int64_t now_ms) {
    if (s->still_ms > 3000) s->rested = true;
    if (s->rested && !sample->still) {
        float alignment = rest_alignment(s);
        s->turned_ms = alignment < PICKUP_COS ? s->turned_ms + s->sample_ms : 0;
        if (s->turned_ms >= 260 && now_ms - s->last_pickup_ms > 6000 && now_ms - s->last_shake_ms > 3000) {
            s->last_pickup_ms = now_ms;
            ESP_LOGI(TAG, "picked up after %d s alone (turned %.0f deg)", s->rest_length_ms / 1000,
                     acosf(clampf(alignment, -1, 1)) * 57.3f);
            app_post(EV_PICKUP, s->rest_length_ms / 1000, 0);
            s->rested = false;
        }
        if (!s->stirred_ms) s->stirred_ms = now_ms;
        else if (now_ms - s->stirred_ms > 3000) s->rested = false;
    }
    if (sample->still) {
        s->stirred_ms = 0;
        s->turned_ms = 0;
    }
}

static void update_motion(input_state_t *s, int64_t now_ms) {
    if (!s->imu || s->tick % s->sample_every != 0) return;
    motion_sample_t sample;
    if (!read_motion_sample(s, &sample)) return;
    update_viewpoint(s, &sample);
    update_rest_state(s, &sample);
    log_motion(s, &sample, now_ms);
    detect_shake(s, &sample, now_ms);
    detect_pickup(s, &sample, now_ms);
}

static volatile bool s_dark;
static TaskHandle_t s_input_task;
void input_set_dark(bool dark) { s_dark = dark; }

// Dark: a key or the touch controller pulls its line low and wakes the input task (also out of light
// sleep). The handler disarms its pin; the task arms it again once the line is high.
static const gpio_num_t k_wake_pins[] = {PIN_KEY, PIN_BOOT, PIN_TP_INT};
static void wake_isr(void *pin) {
    BaseType_t woken = pdFALSE;
    gpio_intr_disable((gpio_num_t)(intptr_t)pin);
    vTaskNotifyGiveFromISR(s_input_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void wake_pins_init(void) {
    gpio_install_isr_service(0);  // may already be installed
    for (size_t i = 0; i < sizeof k_wake_pins / sizeof k_wake_pins[0]; i++) {
        gpio_isr_handler_add(k_wake_pins[i], wake_isr, (void *)(intptr_t)k_wake_pins[i]);
        gpio_wakeup_enable(k_wake_pins[i], GPIO_INTR_LOW_LEVEL);
        gpio_intr_disable(k_wake_pins[i]);
    }
    esp_sleep_enable_gpio_wakeup();
}

static void set_wake_pins(bool armed) {
    for (size_t i = 0; i < sizeof k_wake_pins / sizeof k_wake_pins[0]; i++) {
        if (armed && gpio_get_level(k_wake_pins[i])) gpio_intr_enable(k_wake_pins[i]);
        else if (!armed) gpio_intr_disable(k_wake_pins[i]);
    }
}

// Lit (or just after a press): a 10 ms tick, samples every 20 ms. Dark: sleeps until a wake pin fires or
// 40 ms pass, samples every 40 ms and reads the PMIC's key flags every 200 ms as before.
static void input_pace(input_state_t *s) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_dark && now_ms >= s->busy_until_ms) {
        set_wake_pins(true);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40))) s->busy_until_ms = esp_timer_get_time() / 1000 + BURST_MS;
    } else {
        set_wake_pins(false);
        ulTaskNotifyTake(pdTRUE, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool slow = s_dark && esp_timer_get_time() / 1000 >= s->busy_until_ms;
    s->sample_ms = slow ? 40 : 20;
    s->sample_every = slow ? 1 : 2;
    s->pmic_every = slow ? 5 : 20;
}

static void input_task(void *arg) {
    (void)arg;
    input_state_t state = {
        .gravity = {0, 0, 1},
        .rest = {0, 0, 1},
        .last_shake_ms = -10000,
        .last_pickup_ms = -10000,
    };
    state.imu = imu_init();
    viewpoint_init(&state.viewpoint);
    wake_pins_init();

    while (1) {
        input_pace(&state);
        int64_t now_ms = esp_timer_get_time() / 1000;
        state.tick++;
        update_ptt_key(&state);
        update_boot_key(&state, now_ms);
        if (state.key || state.boot) state.busy_until_ms = now_ms + BURST_MS;
        update_power_key(&state);
        update_touch(&state, now_ms);
        update_motion(&state, now_ms);
    }
}

void input_init(void) { ESP_ERROR_CHECK(xTaskCreate(input_task, "input", 4096, NULL, 9, &s_input_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM); }
