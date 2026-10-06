#include <assert.h>
#include <stdio.h>
#include "app_mailbox.h"

int main(void) {
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
    // The inbox remains bounded and delivery order survives the ticket counter wrapping.
    box.next = UINT32_MAX - 1;
    for (unsigned n = 0; n < APP_MAILBOX_SLOTS; n++) app_mailbox_put(&box, n, (app_ev_t){1, (int)n, 0, 0});
    for (unsigned n = 0; n < APP_MAILBOX_SLOTS; n++) assert(app_mailbox_take(&box, &event) && event.a == (int)n);
    for (unsigned n = 0; n < 10000; n++) app_mailbox_put(&box, 2, (app_ev_t){3, (int)n, 0, 0});
    assert(app_mailbox_take(&box, &event) && event.a == 9999);
    assert(!app_mailbox_take(&box, &event));
    puts("app-mailbox: bounded coalescing, delivery/release order and counter wrap passed");
}
