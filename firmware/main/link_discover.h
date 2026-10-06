// Where the Wi-Fi WebSocket goes. wss:// and ws:// are used as saved; kubik://host[:port]
// becomes wss://host:port/kubik/v1; an empty server is found on this network by a UDP
// broadcast (docs/protocol.md "Discovery"), without blocking the link manager.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LINK_LAN_PORT 18790

typedef enum { TARGET_WAITING, TARGET_READY, TARGET_NOT_FOUND } link_target_t;

// Called every manager tick until it is no longer TARGET_WAITING; `uri` is set when READY.
link_target_t link_target(uint32_t now_ms, char *uri, size_t cap);
// Abandons a discovery in progress (Wi-Fi went away).
void link_target_stop(void);
