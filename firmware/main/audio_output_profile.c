#include "audio_output_profile.h"
#include "freertos/FreeRTOS.h"

static audio_output_profile_t s_profile;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
audio_output_profile_t audio_output_profile_read(bool reset) {
    portENTER_CRITICAL(&s_lock);
    audio_output_profile_t result = s_profile;
    if (reset) s_profile = (audio_output_profile_t){0};
    portEXIT_CRITICAL(&s_lock);
    return result;
}
void audio_output_profile_note(int64_t fed, uint32_t mix_us, bool audible, bool failed) {
    portENTER_CRITICAL(&s_lock);
    audio_output_profile_record(&s_profile, fed, mix_us, audible, failed);
    portEXIT_CRITICAL(&s_lock);
}
