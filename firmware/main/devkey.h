// Kubik's own identity: a P-256 key pair made on the device the first time it
// boots. The private half never leaves it (not over USB, not in the portal);
// OpenClaw learns only the public key and approves it once by a pairing code.
// Each connection proves the key by signing a fresh server challenge, so a
// proxy in the middle sees nothing it could replay.
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool devkey_init(void);           // loads or creates the key (NVS); false on failure
const char *devkey_public(void);  // base64 of the 65-byte uncompressed point ("" if none)
// Signs "kubik-auth-v5\n<nonce>\n<device>\n<public key>\n<bind>" (SHA-256, DER) as base64;
// <bind> names the transport the server was reached over (link_pin.h).
bool devkey_sign_challenge(const char *nonce, const char *bind, char *sig_b64, size_t cap);
