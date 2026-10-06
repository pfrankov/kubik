// Firmware identity is selected at build time; only the native simulator can switch.
#pragma once
typedef enum { CHARACTER_PLUSH = 0, CHARACTER_TESS, CHARACTER_COUNT } character_t;
static inline const char *character_name(character_t c) { return c == CHARACTER_TESS ? "Tess" : "Plush"; }

#ifndef KUBIK_CHARACTER
#define KUBIK_CHARACTER 1  // Tess is the default build.
#endif
