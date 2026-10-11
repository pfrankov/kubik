#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

bool app_game_probe(cJSON *request, char *reply, size_t cap);
// USB test hook: a validated tilt (each axis -1..1) that replaces the sensor's for at most 10 s, then expires.
bool app_game_probe_gravity(float *x, float *y);
