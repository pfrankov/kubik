// The ending melody of a sky: sung back from how it was played. It walks the stars in the order the player made them
// full (any never made full follow, nearest first), each star's note longer the longer it took to light, with a few
// stepwise passing tones between, then answers and comes home to the tonic. No heap, no state:
// the same world and seed always give the same bytes.
#pragma once
#include "tess_sky.h"

// (sky_note_t and sky_melody_t are in tess_sky.h: a sky keeps its melody.)
void sky_melody_compose(const sky_world_t *w, const sky_played_t *played, uint32_t seed, sky_melody_t *m);
float sky_melody_start_ms(const sky_melody_t *m, int i);  // when note i begins, ms from the melody's start
float sky_melody_length_ms(const sky_melody_t *m);        // when the last note ends
