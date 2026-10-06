// Server transport. The same protocol (docs/protocol.md) runs over either
//   * USB: framed stream on the USB Serial/JTAG port, bridged by tools/usb-bridge;
//   * Wi-Fi: WebSocket to the kubik channel plugin (found and pinned by link_pin/link_discover).
// Wi-Fi is the primary route; USB is the fallback, used only after the bridge receives an authenticated server welcome.
// HOST_HELLO v2 separates physical presence from routed-service availability.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// PCM bounds before IMA encoding: speech 100 ms, microphone 40 ms at 24 kHz mono.
// Microphone wire frame is 485 bytes. Audio has kind/tag bytes; USB adds an epoch.
#define LINK_JSON_MAX 4096
#define LINK_SPEECH_PCM_MAX 4800
#define LINK_MIC_PCM_MAX 1920
#define LINK_AUDIO_HEADER_BYTES 2
#define LINK_USB_EPOCH_BYTES 4
#define LINK_WS_RX_MAX (LINK_SPEECH_PCM_MAX + LINK_AUDIO_HEADER_BYTES)
#define LINK_USB_PAYLOAD_MAX (LINK_WS_RX_MAX + LINK_USB_EPOCH_BYTES)
// Includes a text sentinel and one alignment byte.
#define LINK_USB_RX_STORAGE (LINK_USB_PAYLOAD_MAX + 2)

typedef struct {
    void (*on_json)(const char *json, size_t len);
    void (*on_audio)(uint8_t kind, uint8_t tag, const uint8_t *pcm, size_t len);
    void (*on_link)(bool up);
    // USB config command (frame 0x21); write a JSON reply into `reply`.
    void (*on_config)(const char *json, size_t len, char *reply, size_t cap);
    // OpenClaw does not know this Kubik yet: show `code` for the owner to approve.
    // Called again while waiting; the same connection then gets its welcome.
    void (*on_pair)(const char *code);
} link_handlers_t;

// Why the last server connection did not work out (for the screen).
typedef enum {
    LINK_OK = 0,
    LINK_NO_SERVER,  // DNS/TCP/TLS failed: address wrong or OpenClaw down
    LINK_NO_PLUGIN,  // a web server answered but no Kubik endpoint there
    LINK_REFUSED,    // OpenClaw refused this Kubik (revoked, or a bad key)
    LINK_PAIRING,    // waiting for the owner to approve the pairing code
    LINK_KEY_CHANGED,  // LAN mode: the server's TLS key is not the pinned one
} link_problem_t;
link_problem_t link_last_problem(void);

void link_init(const link_handlers_t *h);
void link_pause_wifi(uint32_t ms);  // test hook: no Wi-Fi route for ms, then it resumes by itself
bool link_send_duplex_in_session(uint8_t turn, const uint8_t *ima, uint32_t session);
void link_set_wifi_allowed(bool allowed);
bool link_wifi_stopped(void);  // false while Wi-Fi transport must remain stopped
void link_endpoint(char *out, size_t cap); // Current Wi-Fi WebSocket endpoint; empty without that route.
const char *link_via(void);  // "usb", "wifi" or "none"
bool link_send_json(const char *json);
// Connection identity is captured in the receive callback; stale UI receipts cannot cross reconnects.
uint32_t link_session(void);
bool link_send_json_in_session(const char *json, uint32_t session);
bool link_send_mic_in_session(uint8_t tag, const int16_t *pcm, size_t len, uint32_t session, const uint8_t *ima);
bool link_usb_host_present(void);
