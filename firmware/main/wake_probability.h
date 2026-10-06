#pragma once
#include <stdbool.h>
// The selected model emits uint8 probability with scale 1/256.
enum { WAKE_PROBABILITY_WINDOW = 5, WAKE_PROBABILITY_PERCENT = 97, WAKE_WARMUP_FEATURES = 100 };
static inline bool wake_should_trigger(unsigned features, unsigned sum) {
    return features >= WAKE_WARMUP_FEATURES &&
        sum * 100 > WAKE_PROBABILITY_PERCENT * 256 * WAKE_PROBABILITY_WINDOW;
}
