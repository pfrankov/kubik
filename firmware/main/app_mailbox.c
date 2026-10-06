#include "app_mailbox.h"

void app_mailbox_put(app_mailbox_t *box, unsigned slot, app_ev_t event) {
    if (slot >= APP_MAILBOX_SLOTS) return;
    box->events[slot] = event;
    box->order[slot] = box->next++;
    box->pending |= (uint8_t)(1u << slot);
}

bool app_mailbox_take(app_mailbox_t *box, app_ev_t *event) {
    int oldest = -1;
    for (unsigned slot = 0; slot < APP_MAILBOX_SLOTS; slot++) {
        if (!(box->pending & (1u << slot))) continue;
        if (oldest < 0 || (int32_t)(box->order[slot] - box->order[oldest]) < 0) oldest = (int)slot;
    }
    if (oldest < 0) return false;
    *event = box->events[oldest];
    box->pending &= (uint8_t)~(1u << oldest);
    return true;
}
