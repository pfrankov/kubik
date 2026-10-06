#pragma once
// Heap checkpoints: one log line with the free internal heap, its largest block and the lowest points seen
// (since boot, and since the previous checkpoint in a debug build). Cheap enough to stay in every build.
void hp_mark(const char *phase);

// Counts every allocation failure even after bounded detailed logging stops.
unsigned hp_allocation_failures(void);
