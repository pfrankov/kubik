// The frames of the v5 handshake (docs/protocol.md) that depend on no link state.
#pragma once

#include <stdbool.h>
#include <stddef.h>

bool link_is_welcome(const char *text, size_t len);
// {"t":"hello","v":5,...}; false until the device key exists.
bool link_make_hello(char *buf, size_t cap);
// {"t":"auth","sig":...}: the device key signs the nonce and the transport binding.
bool link_make_auth(const char *nonce, const char *bind, char *buf, size_t cap);
