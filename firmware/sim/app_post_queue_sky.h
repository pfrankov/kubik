// Skies' progress in the app task's flush: written only once the sky has settled, and then just once.
// (Included by app_post_queue_test.c, which holds the fake settings and the app task.)
static uint32_t fake_sky;
static bool fake_sky_settled = true;
static int sky_save_calls;
static uint32_t tess_games_sky_progress(const void *f, bool *settled) { (void)f; *settled = fake_sky_settled; return fake_sky; }
static esp_err_t settings_save_sky_progress(uint32_t progress) { sky_save_calls++; g_settings.sky_progress = progress; return ESP_OK; }
static void app_task_pass(void);
static void test_sky_progress_flush_waits_for_the_sky_to_settle(void) {
    fake_sky = 0x42; fake_sky_settled = false;
    app_task_pass();
    assert(sky_save_calls == 0);
    fake_sky_settled = true;
    app_task_pass(); app_task_pass();
    assert(sky_save_calls == 1 && g_settings.sky_progress == 0x42);
}
