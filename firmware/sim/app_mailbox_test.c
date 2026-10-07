#include <assert.h>
#include <stdio.h>
#include "app_mailbox.h"

static void test_original_delivery_order(void) {
    app_mailbox_t box = {0};
    app_ev_t event;
    assert(!app_mailbox_take(&box, &event));
    // Latest full-card update replaces its old revision; start/end retain their order.
    app_mailbox_put(&box, 0, (app_ev_t){1, 7, 1, 0});
    app_mailbox_put(&box, 2, (app_ev_t){3, 10, 0, 0});
    app_mailbox_put(&box, 1, (app_ev_t){2, 7, 0, 0});
    app_mailbox_put(&box, 2, (app_ev_t){3, 11, 0, 0});
    app_mailbox_put(&box, 3, (app_ev_t){4, 0, 0, 0});
    assert(app_mailbox_take(&box, &event) && event.type == 1 && event.a == 7);
    assert(app_mailbox_take(&box, &event) && event.type == 2 && event.a == 7);
    assert(app_mailbox_take(&box, &event) && event.type == 3 && event.a == 11);
    assert(app_mailbox_take(&box, &event) && event.type == 4);
    assert(!app_mailbox_take(&box, &event));
}

static void test_control_slot_order_and_coalescing(void) {
    app_mailbox_t ordered = {0};
    app_ev_t event;
    // The added control slots keep key/speech ordering and coalesce welcome updates.
    app_mailbox_put(&ordered, 0, (app_ev_t){10, 1, 0, 1}); // speech
    app_mailbox_put(&ordered, 3, (app_ev_t){11, 2, 0, 1}); // KEY release
    app_mailbox_put(&ordered, 8, (app_ev_t){12, 0, 0, 0}); // disconnect
    app_mailbox_put(&ordered, 9, (app_ev_t){13, 70, 0, 1}); // stale welcome
    app_mailbox_put(&ordered, 9, (app_ev_t){13, 80, 1, 2}); // latest welcome
    assert(ordered.pending == (uint16_t)((1u << 0) | (1u << 3) | (1u << 8) | (1u << 9)));
    assert(app_mailbox_take(&ordered, &event) && event.type == 10);
    assert(app_mailbox_take(&ordered, &event) && event.type == 11);
    assert(app_mailbox_take(&ordered, &event) && event.type == 12);
    assert(app_mailbox_take(&ordered, &event) && event.type == 13 && event.a == 80 && event.session == 2);
    assert(!app_mailbox_take(&ordered, &event));
}

static void test_all_slots_and_counter_wrap(void) {
    app_mailbox_t box = {0};
    app_ev_t event;
    box.next = UINT32_MAX - 1;
    for (unsigned n = 0; n < APP_MAILBOX_SLOTS; n++) app_mailbox_put(&box, n, (app_ev_t){1, (int)n, 0, 0});
    for (unsigned n = 0; n < APP_MAILBOX_SLOTS; n++) assert(app_mailbox_take(&box, &event) && event.a == (int)n);
    assert(!app_mailbox_take(&box, &event));
}

static void test_latest_same_slot_wins(void) {
    app_mailbox_t box = {0};
    app_ev_t event;
    for (unsigned n = 0; n < 10000; n++) app_mailbox_put(&box, 2, (app_ev_t){3, (int)n, 0, 0});
    assert(app_mailbox_take(&box, &event) && event.a == 9999);
    assert(!app_mailbox_take(&box, &event));
}

int main(void) {
    test_original_delivery_order();
    test_control_slot_order_and_coalescing();
    test_all_slots_and_counter_wrap();
    test_latest_same_slot_wins();
    puts("app-mailbox: bounded coalescing, 10-slot ordering and counter wrap passed");
}
