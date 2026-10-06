// Which server this Kubik trusts, and the transport binding it signs (docs/protocol.md).
//   LAN mode (server "" or kubik://): the plugin's self-signed TLS key is pinned by the
//   SHA-256 of its SubjectPublicKeyInfo. The first key is trusted on first use and kept only
//   after the server's welcome; a different key later fails the handshake ("server key
//   changed") until the user saves the connection again.
//   CA mode (wss://) binds "ca:<host>", the host of the server setting (lowercase, no port, no IPv6 brackets), so
//   a device pointed at the wrong server has its handshake refused by the right one; device Wi-Fi always uses TLS.
// Over USB the bridge is the TLS client and reports the binding it saw (frame 0x23);
// LAN mode checks it against the pin exactly like a direct handshake.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LINK_BIND_MAX 128  // "ca:", a host of at most 123 characters (docs/protocol.md; or 64 hex characters) and the terminator

typedef enum { SERVER_LAN_DISCOVER, SERVER_LAN_FIXED, SERVER_CA, SERVER_INVALID } server_mode_t;
server_mode_t link_server_mode(const char *url);

// esp_websocket_client crt_bundle_attach hook for LAN mode: installs the pinning verify callback.
esp_err_t link_pin_attach(void *ssl_conf);
// A new Wi-Fi connection attempt: forget the key the previous one saw.
void link_pin_begin_wifi(void);
// USB frame 0x23 for `epoch`: remembers the bind unless it contradicts the pin.
void link_pin_usb_bind(uint32_t epoch, const char *bind, size_t len);
// The bind to sign for a challenge on this route; false when there is none (no auth then).
bool link_pin_bind(bool usb, uint32_t epoch, char out[LINK_BIND_MAX]);
// After a welcome on this route: LAN mode keeps the key it trusted on first use.
void link_pin_keep(bool usb, uint32_t epoch);
// True once after a handshake or USB bind met a different key than the pinned one.
bool link_pin_take_mismatch(void);
