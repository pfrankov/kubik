#include "qr.h"

#include <stdbool.h>
#include "qrcodegen.h"

int qr_make(const char *text, uint8_t mods[QR_MAX_N * QR_MAX_N]) {
    static uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(6)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!qrcodegen_encodeText(text, tmp, code, qrcodegen_Ecc_MEDIUM, 1, 6, qrcodegen_Mask_AUTO, true)) return 0;
    int n = qrcodegen_getSize(code);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) mods[y * n + x] = qrcodegen_getModule(code, x, y) ? 1 : 0;
    return n;
}
