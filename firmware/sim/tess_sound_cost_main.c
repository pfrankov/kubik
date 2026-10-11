// Identical cost inputs run under sanitizers for safety and without them for timing.
#include "tess_sound_probe.h"
void check_cost(tess_sound_t *sound, bool enforce_budget);
int main(void) {
    static tess_sound_t sound;
#ifdef KUBIK_COST_INSTRUMENTED
    check_cost(&sound, false);
#else
    check_cost(&sound, true);
#endif
    return 0;
}
