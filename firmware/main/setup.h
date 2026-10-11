// Setup from a phone, no app and no computer: Kubik opens its own Wi-Fi
// network "Kubik-XXXX" (WPA2, password only in the QR on its screen), phones
// that join it open the second QR in their browser, and the chosen home
// network is tried beside the access point before anything is saved.
//
// Runs inside the normal firmware (face, sounds and USB keep working):
//   first boot (no Wi-Fi saved), Status → Network → Wi-Fi, or the saved network
//   missing for a minute (closes by itself if that network comes back).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SETUP_OFF = 0,
    SETUP_JOIN,    // QR: join Kubik's Wi-Fi
    SETUP_OPEN,    // a phone joined: QR with the page address
    SETUP_TRYING,  // joining the chosen home network
    SETUP_DONE,    // saved; the app restarts after the good news
    SETUP_FAILED,  // shown for a few seconds, then back to OPEN/JOIN
} setup_phase_t;

typedef enum { SETUP_BY_USER = 0, SETUP_FIRST, SETUP_LOST } setup_reason_t;
typedef enum {
    SETUP_ERR_NONE = 0,
    SETUP_ERR_PASSWORD,
    SETUP_ERR_NOT_FOUND,
    SETUP_ERR_OTHER,
    SETUP_ERR_STORAGE,
} setup_error_t;

void setup_start(setup_reason_t why);
void setup_stop(void);
// Atomically refuse a new connection request while automatic recovery closes.
bool setup_claim_idle_close(void);
bool setup_active(void);
// Advances the state machine (app task). Returns the phase after this step;
// SETUP_OFF after a lost network came back by itself.
setup_phase_t setup_poll(void);
// Names shown as text on the card: Kubik's network, its password (the same
// secret the QR carries) and the home network being tried or just joined ("" when none).
// `error` is the reason of the last failed attempt (setup_error_t).
typedef struct { char ap_ssid[16], ap_pass[9], trying[33]; uint8_t error; } setup_labels_t;
void setup_labels(setup_labels_t *out);
// What the phone did since the last call, for the app task to answer with a sound (the HTTP task cannot).
enum { SETUP_EV_PAGE = 1, SETUP_EV_SCAN = 2, SETUP_EV_SEND = 4 };
uint8_t setup_take_events(void);
// QR shown on the card (NULL while there is none) and its progress step 1..2.
const uint8_t *setup_qr(int *n, int *step);
