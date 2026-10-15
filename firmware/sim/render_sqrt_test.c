#include <assert.h>
#include <stdio.h>
#include "../main/render_raster.c"

int main(void) {
    for (int i = 0; i <= 1024; i++) {
        volatile float input = (float)i;
        uint32_t original = (uint32_t)(sqrtf(input) * 2048.f + .5f);
        assert(s_sqrt_t[i] == (original > 65535 ? 65535 : original));
    }
    puts("render square-root seeds: all 1025 flash entries equal the original runtime binary32 arithmetic");
}
