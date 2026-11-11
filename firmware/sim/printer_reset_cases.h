#pragma once
static bool printer_reset_idle = true;
static unsigned printer_guard_resets;
static bool printer_protocol_reset_ready(void) { return printer_reset_idle; }
static void printer_guard_reset(void) { printer_guard_resets++; }
static void reset_harness(void);
static void test_printer_reset_guard(void) {
    reset_harness();
    g_settings.tess_progress = g_face.tess_games.progress = 7;
    unsigned before = printer_guard_resets;
    printer_reset_idle = false;
    assert(factory_reset() == ESP_ERR_INVALID_STATE);
    assert(g_settings.tess_progress == 7 && g_face.tess_games.progress == 7);
    assert(!s_queue.count && printer_guard_resets == before);
    printer_reset_idle = true; reset_result = ESP_FAIL;
    assert(factory_reset() == ESP_FAIL);
    assert(g_settings.tess_progress == 7 && printer_guard_resets == before);
    reset_result = ESP_OK;
    assert(factory_reset() == ESP_OK);
    assert(!g_settings.tess_progress && printer_guard_resets == before + 1);
    assert(s_queue.count == 1);
}
