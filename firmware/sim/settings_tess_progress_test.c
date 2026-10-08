#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "../main/settings.h"

extern unsigned read_calls, write_calls, commit_calls, progress_write_calls;
extern bool fail_commit;
extern const char *fail_write;
extern bool settings_test_has(const char *key);
static pthread_mutex_t stale_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stale_cond = PTHREAD_COND_INITIALIZER;
static bool snapshot_taken, resume_save;
static esp_err_t stale_result;

static void *save_snapshot_after_reset(void *context) {
    uint8_t snapshot = *(uint8_t *)context;
    pthread_mutex_lock(&stale_lock);
    snapshot_taken = true;
    pthread_cond_broadcast(&stale_cond);
    while (!resume_save) pthread_cond_wait(&stale_cond, &stale_lock);
    pthread_mutex_unlock(&stale_lock);
    stale_result = settings_save_tess_progress(snapshot);
    return NULL;
}
static void reset_between_snapshot_and_save(void) {
    uint8_t snapshot = g_settings.tess_progress;
    pthread_t writer;
    assert(pthread_create(&writer, NULL, save_snapshot_after_reset, &snapshot) == 0);
    pthread_mutex_lock(&stale_lock);
    while (!snapshot_taken) pthread_cond_wait(&stale_cond, &stale_lock);
    pthread_mutex_unlock(&stale_lock);
    assert(settings_factory_reset() == ESP_OK);
    unsigned writes = write_calls;
    pthread_mutex_lock(&stale_lock);
    resume_save = true;
    pthread_cond_broadcast(&stale_cond);
    pthread_mutex_unlock(&stale_lock);
    assert(pthread_join(writer, NULL) == 0);
    assert(stale_result == ESP_ERR_INVALID_STATE);
    assert(write_calls == writes && !settings_test_has("tess_progress") && g_settings.tess_progress == 0);
}

static void test_progress_merge_and_noop(void) {
    // One new earned bit is written and published; repeats and stale snapshots do no I/O.
    unsigned writes = write_calls, commits = commit_calls, reads = read_calls;
    assert(settings_save_tess_progress(0x01) == ESP_OK);
    assert(g_settings.tess_progress == 0x01 && settings_test_has("tess_progress"));
    assert(write_calls == writes + 1 && commit_calls == commits + 1);
    writes = write_calls; commits = commit_calls;
    assert(settings_save_tess_progress(0x01) == ESP_OK);
    assert(settings_save_tess_progress(0x00) == ESP_OK);
    assert(write_calls == writes && commit_calls == commits && read_calls == reads + 1);
    assert(g_settings.tess_progress == 0x01);

    // Supersets merge with durable state; they can never clear a previously earned bit.
    assert(settings_save_tess_progress(0x04) == ESP_OK);
    assert(g_settings.tess_progress == 0x05);
    writes = write_calls; commits = commit_calls; reads = read_calls;
    assert(settings_save_tess_progress(0x01) == ESP_OK);
    assert(write_calls == writes && commit_calls == commits && read_calls == reads);
    settings_load();
    assert(g_settings.tess_progress == 0x05);
}

static void test_progress_rejects_and_recovers(void) {
    // Invalid bits are rejected before NVS access.
    unsigned writes = write_calls, commits = commit_calls, reads = read_calls;
    assert(settings_save_tess_progress(0x40) == ESP_ERR_INVALID_ARG);
    assert(write_calls == writes && commit_calls == commits && read_calls == reads &&
           g_settings.tess_progress == 0x05);

    // A failed write/commit leaves RAM and durable state at the last committed mask.
    fail_write = "tess_progress";
    assert(settings_save_tess_progress(0x09) == ESP_FAIL);
    fail_write = NULL;
    assert(g_settings.tess_progress == 0x05);
    settings_load();
    assert(g_settings.tess_progress == 0x05);
    fail_commit = true;
    assert(settings_save_tess_progress(0x0d) == ESP_FAIL);
    fail_commit = false;
    assert(g_settings.tess_progress == 0x05);
    settings_load();
    assert(g_settings.tess_progress == 0x05);
    assert(settings_save_tess_progress(0x08) == ESP_OK);
    assert(g_settings.tess_progress == 0x0d);
}

static void test_progress_independent_reset(void) {
    // Generic preference saves leave the dedicated milestone key untouched.
    unsigned writes = write_calls, commits = commit_calls;
    unsigned progress_writes = progress_write_calls;
    settings_save();
    assert(write_calls > writes && commit_calls == commits + 1);
    assert(progress_write_calls == progress_writes && g_settings.tess_progress == 0x0d);
    settings_load();
    assert(g_settings.tess_progress == 0x0d && !strcmp(g_settings.name, "Existing") &&
           g_settings.volume == 42);

    reset_between_snapshot_and_save();
    assert(!settings_test_has("tess_progress") && g_settings.tess_progress == 0);
}

static void test_corrupt_progress_is_recoverable(void) {
    // Ignore corrupt saved bits at boot, then allow the next legitimate win to heal the key.
    assert(nvs_set_u8(1, "tess_progress", 0x81) == ESP_OK);
    settings_load();
    assert(g_settings.tess_progress == 0);
    assert(settings_save_tess_progress(0x01) == ESP_OK);
    assert(g_settings.tess_progress == 0x01);
    settings_load();
    assert(g_settings.tess_progress == 0x01);
}

void settings_test_tess_progress_storage(void) {
    nvs_flash_erase();
    assert(nvs_set_str(1, "name", "Existing") == ESP_OK);
    assert(nvs_set_i32(1, "volume", 42) == ESP_OK);
    settings_load();
    assert(g_settings.tess_progress == 0 && !settings_test_has("tess_progress"));
    assert(!strcmp(g_settings.name, "Existing") && g_settings.volume == 42);
    test_progress_merge_and_noop();
    test_progress_rejects_and_recovers();
    test_progress_independent_reset();
    test_corrupt_progress_is_recoverable();
}
