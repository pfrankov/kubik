// Shared Settings/Sound semantics; stored and reported levels remain independent.
#pragma once
static inline int sound_level_bound(int value) { return value < 0 ? 0 : value > 100 ? 100 : value; }
static inline int sound_level_mean(int speech, int interface) { return (speech + interface + 1) / 2; }

// Stored channel levels are validated 0..100 by Settings and Screen Lab.
void sound_level_link(int *speech, int *interface, int target);
