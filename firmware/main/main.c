#include <stdatomic.h>
#include "app_lab.h"
// Kubik: a push-to-talk desk companion for OpenClaw.
#include <string.h>

#include "app.h"
#include "assets.h"
#include "audio.h"
#include "board.h"
#include "canvas.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "present.h"
#include "render.h"
#include "settings.h"
#include "muse_store.h"
#include "esp_bt.h"
#include "wifi.h"
#include "setup.h"
#include "tls_mem.h"
#include "tess.h"

static const char *TAG = "main";
atomic_bool g_mode_report;



static volatile int s_req_brightness = -1, s_req_fade_ms;
static volatile int s_req_power = -1;
static TaskHandle_t s_display_task;

void disp_brightness_fade(int level, int ms) {
    s_req_fade_ms = ms < 0 ? 0 : ms;
    s_req_brightness = level < 0 ? 0 : level > 255 ? 255 : level;
}
void disp_wake(void) {
    if (s_display_task) xTaskNotifyGive(s_display_task);
}
void disp_power(bool on) {
    s_req_power = on;
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

// Automatic light sleep is configured once; this lock keeps it off except while the app allows it.
static esp_pm_lock_handle_t s_awake_lock;
static atomic_bool s_light_sleep_ok;
bool power_light_sleep_enabled(void) { return atomic_load(&s_light_sleep_ok); }
void power_light_sleep_allow(bool allow) {
    if (allow == s_light_sleep_ok) return;
    s_light_sleep_ok = allow;
    ESP_ERROR_CHECK(allow ? esp_pm_lock_release(s_awake_lock) : esp_pm_lock_acquire(s_awake_lock));
}

static volatile bool s_req_reinit;
void disp_reinit(void) { s_req_reinit = true; }


#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
// CPU share per task since the last call (profiling builds only).
static void log_cpu(void) {
    static TaskStatus_t st[28];
    static uint32_t last_rt[28], last_num[28], last_total;
    uint32_t total;
    int n = uxTaskGetSystemState(st, 28, &total);
    uint32_t span = total - last_total;
    last_total = total;
    char line[400];
    int at = 0;
    for (int i = 0; i < n && span; i++) {
        uint32_t prev = 0;
        for (int k = 0; k < 28; k++) if (last_num[k] == st[i].xTaskNumber) { prev = last_rt[k]; break; }
        uint32_t tenths = (uint32_t)((uint64_t)(st[i].ulRunTimeCounter - prev) * 1000 / span);
        if (tenths >= 1 && at < (int)sizeof line - 24)
            at += snprintf(line + at, sizeof line - at, " %s %lu.%lu%%", st[i].pcTaskName, (unsigned long)tenths / 10, (unsigned long)tenths % 10);
    }
    for (int i = 0; i < n && i < 28; i++) { last_num[i] = st[i].xTaskNumber; last_rt[i] = st[i].ulRunTimeCounter; }
    ESP_LOGI(TAG, "cpu:%s", line);
#if CONFIG_PM_PROFILING
    esp_pm_dump_locks(stdout);
#endif
}
#endif

static int step_brightness(int requested, int *brightness, int *fade_from, int *fade_to,
                           int64_t *fade_started, int64_t *fade_duration) {
    if (requested >= 0) {
        s_req_brightness = -1;
        *fade_from = *brightness;
        *fade_to = requested;
        *fade_started = esp_timer_get_time();
        *fade_duration = (int64_t)s_req_fade_ms * 1000;
    }

    int next_brightness = -1;
    if (*brightness != *fade_to) {
        float progress = *fade_duration > 0 ? (float)(esp_timer_get_time() - *fade_started) / *fade_duration : 1.f;
        if (progress > 1) progress = 1;
        progress = progress * progress * (3 - 2 * progress);
        int value = (int)(*fade_from + (*fade_to - *fade_from) * progress + (*fade_to > *fade_from ? 0.5f : -0.5f));
        if (progress >= 1) value = *fade_to;
        if (value != *brightness) next_brightness = *brightness = value;
    }
    return next_brightness;
}

static void refresh_display(bool powered) {
    static int64_t refreshed;
    if (!powered || esp_timer_get_time() - refreshed <= 2000000) return;
    refreshed = esp_timer_get_time();
    display_wait_idle();
    display_panel_refresh();
    display_power(powered);
}

static void reinitialize_display(frame_out_t *frame, bool powered, int brightness) {
    if (!s_req_reinit) return;
    s_req_reinit = false;
    display_wait_idle();
    display_panel_init();
    display_set_brightness((uint8_t)brightness);
    if (!powered) display_power(false);
    canvas_reset(&frame->canvas);
    ESP_LOGW(TAG, "panel re-initialised");
}

static void service_display_controls(frame_out_t *frame, bool *powered, int *brightness,
                                     int *fade_from, int *fade_to, int64_t *fade_started,
                                     int64_t *fade_duration) {
    int requested_brightness = s_req_brightness;
    int requested_power = s_req_power;
    int next_brightness = step_brightness(requested_brightness, brightness, fade_from, fade_to,
                                          fade_started, fade_duration);
    if (next_brightness >= 0 || requested_power >= 0) display_wait_idle();
    if (next_brightness >= 0) display_set_brightness((uint8_t)next_brightness);
    if (requested_power >= 0) {
        s_req_power = -1;
        *powered = requested_power;
        display_power(*powered);
    }

    refresh_display(*powered);
    reinitialize_display(frame, *powered, *brightness);
}

static face_mode_t update_scene(scene_t *scene, float elapsed, bool powered) {
    xSemaphoreTake(g_face_mtx, portMAX_DELAY);
    app_face_inputs(&g_face);
    g_face.dark = !powered;
    bool lab = app_lab_frame(scene, elapsed, powered);
    if (!lab) { face_update(&g_face, elapsed); if (powered) face_draw(&g_face, scene); }
    tess_cue_t cue[TESS_CUES];
    float strength[TESS_CUES], position[TESS_CUES];
    int cues = 0;
    while (KUBIK_CHARACTER == CHARACTER_TESS && cues < TESS_CUES) {
        bool next = lab ? app_lab_take_cue(&cue[cues], &strength[cues], &position[cues]) :
                          face_take_cue(&g_face, &cue[cues], &strength[cues], &position[cues]);
        if (!next) break;
        cues++;
    }
    face_mode_t mode = g_face.mode;
    xSemaphoreGive(g_face_mtx);
    for (int i = 0; i < cues; i++) {
        if (lab) app_lab_play_cue(cue[i], strength[i], position[i]);
        else audio_tess_cue(cue[i], strength[i], position[i]);
    }
    return mode;
}

typedef struct {
    int64_t start, sum, max, face, render, image, shapes, prep, glass, present_sum, present_max;
    int64_t hash_seen, pack_seen, send_seen, px_seen, windows_seen;  // f->hash_us / expand_us / px / windows at the end of the previous frame: the per-frame share
    int frames, late;
} frame_stats_t;

// One frame's cost; a frame over its slot gets its own line with the stages, so a stutter can be traced.
static void account_frame(frame_stats_t *st, frame_out_t *f, const render_state_t *rs, int64_t face_us, int64_t render_us,
                          int64_t total_us) {
    st->sum += total_us;
    st->face += face_us;
    st->render += render_us;
    st->image += rs->image_us; st->shapes += rs->shapes_us; st->prep += rs->prep_us; st->glass += rs->glass_us;
    if (total_us > st->max) st->max = total_us;
    st->frames++;
    if (total_us > (1000000 + R_FPS - 1) / R_FPS) {
        st->late++;
        ESP_LOGW(TAG, "slow frame %lld us, mode %d: face %lld, prep %d, image %d, shapes %d, glass %d, hash %lld, pack %lld, send %lld, px %lld in %lld windows",
                 total_us, (int)g_face.mode, face_us, (int)rs->prep_us, (int)rs->image_us, (int)rs->shapes_us, (int)rs->glass_us,
                 f->hash_us - st->hash_seen, f->pack_us - st->pack_seen, f->expand_us - st->send_seen, f->px - st->px_seen, f->windows - st->windows_seen);
    }
    st->hash_seen = f->hash_us;
    st->pack_seen = f->pack_us;
    st->send_seen = f->expand_us;
    st->px_seen = f->px;
    st->windows_seen = f->windows;
}

static void log_frame_stats(frame_stats_t *st, frame_out_t *f, const scene_t *scene, int64_t now) {
    int n = st->frames;
    ESP_LOGI(TAG, "display: %.1f fps (%d frames), %lld us/frame (max %lld, %d over %.1f ms), %lld px/frame in %.1f windows, prims %d, heap %u (min %u), lossless frame max %u/%u bytes (%u over, %u split)",
             n * 1e6f / (now - st->start), n, st->sum / n, st->max, st->late, 1000.0 / R_FPS, f->px / n,
             (double)f->windows / n, scene->n, (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             f->store_max, (unsigned)sizeof f->store.data, f->overflows, f->flushes);
    ESP_LOGI(TAG, "profile us/frame: face %lld, render+send %lld (prep %lld, image %lld, shapes %lld, glass %lld, hash %lld, pack %lld, send %lld)",
             st->face / n, st->render / n, st->prep / n, st->image / n, st->shapes / n, st->glass / n, f->hash_us / n, f->pack_us / n, f->expand_us / n);
    ESP_LOGI(TAG, "present: %lld us/frame max %lld, panel burst (first to last write) %lld us avg, %lld max over %lld frames, frames shown %lld..%lld us apart, character %s, stack free %u",
             st->present_sum / n, st->present_max, f->bursts ? f->burst_sum / f->bursts : 0, f->burst_max, f->bursts, f->gap_min, f->gap_max,
             character_name(KUBIK_CHARACTER), (unsigned)uxTaskGetStackHighWaterMark(NULL));
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    log_cpu();
#endif
#if CONFIG_PM_PROFILING
    esp_pm_dump_locks(stdout);
#endif
    *st = (frame_stats_t){.start = now};
    f->hash_us = f->pack_us = f->expand_us = f->px = f->windows = 0;
    f->burst_sum = f->burst_max = f->bursts = f->gap_min = f->gap_max = 0;
    f->store_max = f->overflows = f->flushes = 0;
    f->heal_px0 = 0;
}


// Sleeps to the next frame's time. Dark: a notification, touch or remote reply can wake the screen at once; the
// 250 ms timeout only advances off-screen state when nothing happens.
static void wait_for_next_frame(bool powered, int fps, TickType_t *wake, int *tick_remainder) {
    if (!powered) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        *wake = xTaskGetTickCount();
        *tick_remainder = 0;
        return;
    }
    *tick_remainder += configTICK_RATE_HZ;
    TickType_t period = *tick_remainder / fps;
    *tick_remainder %= fps;
    int32_t left = (int32_t)(*wake + period - xTaskGetTickCount());
    if (left <= 0) {
        // A missed frame must still yield to idle; never spin trying to catch up.
        vTaskDelay(1);
        *wake = xTaskGetTickCount();
    } else if (ulTaskNotifyTake(pdTRUE, (TickType_t)left)) {
        *wake = xTaskGetTickCount();  // disp_wake(): an input changed the screen; draw it now and keep time from here
        *tick_remainder = 0;
    } else {
        *wake += period;
    }
}

static void report_presented_mode(bool presented, face_mode_t mode, bool requested, face_mode_t *shown) {
    if (!presented) {
        if (requested) atomic_store(&g_mode_report, true);
        return;
    }
    if (mode == *shown && !requested) return;
    *shown = mode;
    ESP_LOGI(TAG, "mode %d on screen", (int)mode); // same clock as the app's "key down"
}

static void display_task(void *arg) {
    static scene_t scene;
    static render_state_t rs;
    static uint16_t bands[2][R_W * R_BAND] __attribute__((aligned(4)));  // the canvas hashes pixel pairs
    frame_out_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(f);
    f->scene = &scene;
    f->bands[0] = bands[0];
    f->bands[1] = bands[1];
    for (int i = 0; i < LCD_PIPE; i++) {
        f->bufs[i] = heap_caps_malloc(LCD_W * OUTPUT_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(f->bufs[i]);
    }
    int tick_remainder = 0;  // fractional tick periods keep the requested frame cadence
    TickType_t wake = xTaskGetTickCount();
    int64_t last = esp_timer_get_time();
    frame_stats_t stats = {.start = last};
    bool powered = true;
    face_mode_t shown_mode = MODE_BOOT;
    esp_pm_lock_handle_t display_speed_lock;
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "display", &display_speed_lock));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(display_speed_lock));
    bool speed_locked = true;
    // Brightness glides between levels (smoothstep), one panel command per frame.
    int brightness = g_settings.brightness, fade_from = brightness, fade_to = brightness;
    int64_t fade_started = 0, fade_duration = 0;
    while (1) {
        service_display_controls(f, &powered, &brightness, &fade_from, &fade_to,
                                 &fade_started, &fade_duration);
        if (powered != speed_locked) {
            ESP_ERROR_CHECK(powered ? esp_pm_lock_acquire(display_speed_lock) : esp_pm_lock_release(display_speed_lock));
            speed_locked = powered;
        }
        int64_t t0 = esp_timer_get_time();
        float dt = (t0 - last) / 1e6f;
        last = t0;
        bool report_mode = powered && atomic_exchange(&g_mode_report, false);
        face_mode_t frame_mode = update_scene(&scene, dt, powered);
        int64_t t_face = esp_timer_get_time();
        bool presented = powered && present_frame(f, &rs, &scene);
        int64_t t1 = esp_timer_get_time();
        if (powered) {
            stats.present_sum += t1 - t_face;
            if (t1 - t_face > stats.present_max) stats.present_max = t1 - t_face;
        }
        account_frame(&stats, f, &rs, t_face - t0, powered ? t1 - t_face : 0, t1 - t0);
        report_presented_mode(presented, frame_mode, report_mode, &shown_mode);
        if (t1 - stats.start > 10000000 || g_perf_flush) {
            g_perf_flush = false;
            log_frame_stats(&stats, f, &scene, t1);
        }
        // Reserve CPU for live microphone TLS/VAD; keep a steady animation cadence.
        int fps = frame_mode == MODE_LISTENING ? 24 : R_FPS;
        wait_for_next_frame(powered, fps, &wake, &tick_remainder);
    }
}

static void initialize_home_face(void) {
    face_init(&g_face);
    if (KUBIK_CHARACTER == CHARACTER_TESS) tess_games_restore(&g_face, g_settings.tess_progress);
}

void app_main(void) {
    tls_mem_init();  // before anything uses mbedTLS
    // The no-sleep lock is held before light sleep is enabled: USB must never see a sleep window at boot.
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "awake", &s_awake_lock));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(s_awake_lock));
    const esp_pm_config_t power = {.max_freq_mhz = 160, .min_freq_mhz = CONFIG_XTAL_FREQ, .light_sleep_enable = true};
    ESP_ERROR_CHECK(esp_pm_configure(&power));
    settings_load();
    muse_state_t muse = muse_store_state();
    // Pairing is the only Bluetooth lifetime; changing agents/pairing restarts.
    if (muse != MUSE_PAIRING) ESP_ERROR_CHECK(esp_bt_mem_release(ESP_BT_MODE_BLE));
    board_init();
    bool setup_requested = settings_take_provisioning();  // one-shot flag from older firmware
    if (muse != MUSE_PAIRING) wifi_start_sta();
    g_face_mtx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(assets_init() ? ESP_OK : ESP_FAIL);
    if (KUBIK_CHARACTER == CHARACTER_PLUSH) {
        assets_sprites(true);
    }
    initialize_home_face();
    g_face.rng ^= (uint32_t)esp_timer_get_time();  // its idle moves differ from boot to boot (the simulator keeps the fixed seed)
    display_set_brightness(g_settings.brightness);
    ESP_ERROR_CHECK(xTaskCreate(display_task, "display", 7168, NULL, 8, &s_display_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    audio_init(g_settings.volume);
    audio_set_ui_volume(g_settings.ui_volume);
    audio_sfx(SFX_BOOT);
    app_start();
    // Out of the box Kubik opens its setup network (an empty server means "find it on this network").
    if (setup_requested || !g_settings.wifi_ssid[0])
        app_post(EV_SETUP_START, g_settings.wifi_ssid[0] ? SETUP_BY_USER : SETUP_FIRST, 0);
    ESP_LOGI(TAG, "Kubik %s up, heap %u", g_device_id, (unsigned)esp_get_free_heap_size());
}
