#include "auto_voice.h"
auto_voice_result_t auto_voice_step(auto_voice_t *s, bool speech, bool muted, unsigned ms) {
    s->elapsed_ms += ms;
    if (s->elapsed_ms >= 60000) return s->heard ? AUTO_SEND : AUTO_EMPTY;
    // Discard the final keyword tail; command speech is measured after the wake boundary.
    if (muted || s->elapsed_ms <= s->warmup_ms) { s->quiet_ms = 0; return AUTO_CONTINUE; }
    if (speech) {
        s->voiced_ms += ms;
        s->quiet_ms = 0;
        if (s->voiced_ms >= 120) s->heard = true;
    } else {
        s->quiet_ms += ms;
        if (!s->heard) s->voiced_ms = 0;
    }
    if (s->heard && s->quiet_ms >= 800) return AUTO_SEND;
    if (!s->heard && s->elapsed_ms >= 8000) return AUTO_EMPTY;
    return AUTO_CONTINUE;
}
