// Words Kubik shows on screen. English only for now: a language is one more
// table here (the font covers Latin and Cyrillic). Icons carry the meaning;
// the words are there only where an icon alone would be ambiguous.
#pragma once

typedef enum {
    STR_NOT_HEARD = 0,     // empty transcript
    STR_TRY_AGAIN,         // speech/agent/voice failed
    STR_NO_ANSWER,         // no reply in time
    STR_OPENCLAW_OFFLINE,  // Wi-Fi is fine, the server is not answering
    STR_WAITING_FOR_TIME,  // WSS waits for a trusted clock
    STR_HOLD_TO_TALK,      // first contact: how to talk
    STR_ADD_TO_OPENCLAW,   // the server has no Kubik plugin
    STR_NOT_ALLOWED,       // the server refused this Kubik
    STR_SERVER_KEY_CHANGED,  // LAN mode: not the OpenClaw Kubik trusted before (save setup again)
    STR_LISTENING,         // recording while the button is held
    STR_PAUSE_TO_SEND,     // automatic/native input ends after a pause
    STR_TAP_TO_SEND,       // recording latched: a press or a tap sends
    STR_THINKING,          // sent; OpenClaw is working on it
    STR_CONNECTED,         // welcome from OpenClaw
    STR_PAIRED,            // the owner approved this Kubik
    STR_STOPPED,           // speech interrupted by a tap
    STR_VOLUME,            // followed by the level, e.g. "Volume 60%"
    STR_SETUP_CLOSED,      // left setup without changes
    // What OpenClaw is busy with (status bubble while it works):
    STR_WORKING,           // some run elsewhere in OpenClaw (not about this Kubik)
    STR_USING_TOOLS,
    STR_CODING,
    STR_BROWSING,
    STR_DEPLOYING,
    STR_BUILDING,
    STR_COMPACTING,        // tidying up its memory (context compaction)
    STR_STILL_WORKING,     // no progress for a while
    STR_TURNING_OFF,       // PWR held: powering off
    // Settings menu (hold BOOT):
    STR_WIFI,
    STR_POWER_OFF,
    STR_RESET,             // factory reset (asks twice)
    STR_COUNT
} str_id_t;

const char *str(str_id_t id);
