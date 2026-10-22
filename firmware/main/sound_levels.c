#include "sound_levels.h"

// Shift both channels equally, handing any excess at a limit to the other channel.
// An odd sum keeps its one-point balance until an endpoint makes that impossible.
void sound_level_link(int *speech, int *interface, int target) {
    int a = *speech, b = *interface;
    int shift = sound_level_bound(target) - sound_level_mean(a, b);
    int sum = a + b + 2 * shift;
    if (sum < 0) sum = 0;
    a = sound_level_bound(a + shift);
    b = sound_level_bound(sum - a);
    *speech = sum - b;
    *interface = b;
}
