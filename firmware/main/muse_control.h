#pragma once
#include <stdbool.h>
bool muse_control_start(void (*notification)(const char *));
bool muse_control_ready(void);
bool muse_control_failed(void);
void muse_control_clear(void);
