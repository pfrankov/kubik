#include <assert.h>
#include <stdio.h>

#define ESP_BT_MODE_BLE 1
static unsigned release_calls, error_checks;
static int esp_bt_mem_release(int mode) {
    assert(mode == ESP_BT_MODE_BLE);
    release_calls++;
    return 0;
}
#define ESP_ERROR_CHECK(result) do { error_checks++; assert((result) == 0); } while (0)

#include "actual_startup.inc"

static void check_state(muse_state_t state, unsigned expected) {
    release_calls = error_checks = 0;
    release_paired_muse_bluetooth(state);
    assert(release_calls == expected && error_checks == expected);
}

int main(void) {
    check_state(MUSE_OFF, 0);
    check_state(MUSE_PAIRING, 0);
    check_state(MUSE_PAIRED, 1);
    check_state((muse_state_t)-1, 0);
    check_state((muse_state_t)99, 0);
    puts("BLE startup actual helper: host/pairing retain, native paired Muse releases once, unknown retains");
}
