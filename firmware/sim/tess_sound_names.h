// The names of the sounds, for the host tools and tests. Header only; not firmware.
#pragma once
#include "../main/audio.h"
static const char *const k_sfx_names[SFX_COUNT] = {"boot", "listen_start", "latch_start", "listen_stop", "think",
    "notify", "error", "not_heard", "connect", "disconnect", "wake", "tap", "giggle", "pet", "dizzy",
    "surprise", "volume", "hello", "screen_on", "screen_off", "setup", "setup_phone", "setup_wait",
    "setup_ok", "setup_fail", "menu_open", "menu_close", "detent", "glint", "arm",
    "disarm", "deny", "page", "dismiss", "power_off"};
static const char *const k_cue_names[TC_COUNT] = {"touch", "fling", "swing", "excite", "rub", "glance", "peek", "invite",
    "dodge", "startle", "content", "mischief", "love", "joy", "sad", "tinkle", "snap", "curious", "playful", "scared",
    "grumpy", "lonely", "settle", "impact"};
