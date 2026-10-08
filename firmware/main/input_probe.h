#pragma once
#include <stdbool.h>

// USB-only diagnostics: one controller sample, expiring after 350 ms.
bool input_probe_sample(bool down, int x, int y);
bool input_probe_read(int *x, int *y);
