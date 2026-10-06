// QR codes for the setup card (portable: also used by the host simulator).
#pragma once
#include <stdint.h>

#define QR_MAX_N 41  // up to version 6: Wi-Fi join and page URLs fit easily

// Encodes `text` (medium error correction, boosted when it fits) into n*n bytes
// of modules (1 = dark). Returns n, or 0 if it does not fit.
int qr_make(const char *text, uint8_t mods[QR_MAX_N * QR_MAX_N]);
