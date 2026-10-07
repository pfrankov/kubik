#include "wake_model.h"
#include "wake_probability.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include "assets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "frontend_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

// Main tensor arena measured on C6 with ESP-NN 1.4.1: 22,860 bytes used.
// ResourceVariables has its own separate 1 KB allocator below.
// Keep 692 bytes above that bound without spending another KB of scarce DMA SRAM.
static constexpr size_t ARENA_BYTES = 23 * 1024, VARIABLES_BYTES = 1024;
// Used only by the SDK's fft_util.c, never by I2S or Wi-Fi DMA allocations.
extern "C" void *kubik_wake_fft_alloc(size_t bytes) {
    return heap_caps_malloc(bytes, MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT);
}
static tflite::MicroMutableOpResolver<13> s_ops;
static bool s_registered;
static uint8_t *s_arena, *s_variables;
static tflite::MicroInterpreter *s_interpreter;
static FrontendState s_frontend;
static unsigned s_stride, s_features, s_window_index;
static uint8_t s_window[WAKE_PROBABILITY_WINDOW];
struct Statistics { uint64_t inference_us; uint32_t max_us, invokes; uint8_t probability, peak; };
static Statistics s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static Statistics statistics_snapshot() {
    portENTER_CRITICAL(&s_stats_lock); Statistics result = s_stats; portEXIT_CRITICAL(&s_stats_lock);
    return result;
}
static esp_pm_lock_handle_t s_clock;
static bool s_clock_held;
static std::atomic_bool s_healthy;

static bool register_state_ops() {
    return s_ops.AddCallOnce() == kTfLiteOk && s_ops.AddVarHandle() == kTfLiteOk &&
           s_ops.AddReshape() == kTfLiteOk && s_ops.AddReadVariable() == kTfLiteOk &&
           s_ops.AddConcatenation() == kTfLiteOk && s_ops.AddStridedSlice() == kTfLiteOk &&
           s_ops.AddAssignVariable() == kTfLiteOk;
}
static bool register_compute_ops() {
    return s_ops.AddConv2D() == kTfLiteOk && s_ops.AddDepthwiseConv2D() == kTfLiteOk &&
           s_ops.AddSplitV() == kTfLiteOk && s_ops.AddFullyConnected() == kTfLiteOk &&
           s_ops.AddLogistic() == kTfLiteOk && s_ops.AddQuantize() == kTfLiteOk;
}
static bool frontend_start() {
    FrontendConfig cfg = {};
    FrontendFillConfigWithDefaults(&cfg);
    cfg.window.size_ms = 30; cfg.window.step_size_ms = 10;
    cfg.filterbank.num_channels = 40;
    cfg.filterbank.lower_band_limit = 125; cfg.filterbank.upper_band_limit = 7500;
    cfg.noise_reduction.smoothing_bits = 10;
    cfg.noise_reduction.even_smoothing = .025f; cfg.noise_reduction.odd_smoothing = .06f;
    cfg.noise_reduction.min_signal_remaining = .05f;
    cfg.pcan_gain_control.enable_pcan = true; cfg.pcan_gain_control.strength = .95f;
    cfg.pcan_gain_control.offset = 80; cfg.pcan_gain_control.gain_bits = 21;
    cfg.log_scale.enable_log = true; cfg.log_scale.scale_shift = 6;
    return FrontendPopulateState(&cfg, &s_frontend, 16000);
}
void wake_model_stop() {
    s_healthy = false;
    delete s_interpreter; s_interpreter = nullptr;
    FrontendFreeStateContents(&s_frontend); s_frontend = {};
    heap_caps_free(s_arena); s_arena = nullptr;
    heap_caps_free(s_variables); s_variables = nullptr;
    if (s_clock_held) { esp_pm_lock_release(s_clock); s_clock_held = false; }
}
static bool interpreter_start(const uint8_t *bytes) {
    s_arena = static_cast<uint8_t *>(heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    // Resource variables are CPU-only; wake releases this LP lease before voice I/O.
    s_variables = static_cast<uint8_t *>(heap_caps_malloc(VARIABLES_BYTES, MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT));
    if (!s_arena || !s_variables) return false;
    auto allocator = tflite::MicroAllocator::Create(s_variables, VARIABLES_BYTES);
    auto variables = tflite::MicroResourceVariables::Create(allocator, 20);
    s_interpreter = new(std::nothrow) tflite::MicroInterpreter(tflite::GetModel(bytes), s_ops,
                                                           s_arena, ARENA_BYTES, variables);
    if (!s_interpreter || s_interpreter->AllocateTensors() != kTfLiteOk) return false;
    const TfLiteTensor *input = s_interpreter->input(0), *output = s_interpreter->output(0);
    return input->type == kTfLiteInt8 && input->dims->size == 3 && input->dims->data[0] == 1 &&
           input->dims->data[1] == 2 && input->dims->data[2] == 40 &&
           output->type == kTfLiteUInt8 && output->bytes == 1;
}
bool wake_model_start() {
    if (!s_clock) ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "tessa", &s_clock));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(s_clock)); s_clock_held = true;
    if (!s_registered) { s_registered = register_state_ops() && register_compute_ops(); }
    uint32_t size = 0;
    const uint8_t *bytes = assets_data("wake/tessa", &size);
    bool valid = s_registered && bytes && size == 63496 && memcmp(bytes + 4, "TFL3", 4) == 0;
    if (!valid || !interpreter_start(bytes) || !frontend_start()) {
        wake_model_stop(); ESP_LOGE("wake", "Tessa model could not start"); return false;
    }
    s_stride = s_features = s_window_index = 0;
    portENTER_CRITICAL(&s_stats_lock); s_stats.probability = 0; portEXIT_CRITICAL(&s_stats_lock);
    memset(s_window, 0, sizeof s_window);
    s_healthy = true;
    ESP_LOGI("wake", "Tessa ready, arena=%u heap=%u", (unsigned)s_interpreter->arena_used_bytes(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return true;
}
static bool features_feed(const FrontendOutput &features) {
    auto input = s_interpreter->input(0);
    for (unsigned i = 0; i < 40; i++) {
        int value = (features.values[i] * 256 + 333) / 666 - 128;
        input->data.int8[s_stride * 40 + i] = static_cast<int8_t>(std::clamp(value, -128, 127));
    }
    ++s_features;
    if (++s_stride != 2) return false;
    s_stride = 0;
    int64_t started = esp_timer_get_time();
    if (s_interpreter->Invoke() != kTfLiteOk) {
        s_healthy = false; ESP_LOGE("wake", "Tessa inference failed"); return false;
    }
    uint32_t elapsed = esp_timer_get_time() - started;
    s_window[s_window_index++ % WAKE_PROBABILITY_WINDOW] = s_interpreter->output(0)->data.uint8[0];
    unsigned sum = 0; for (uint8_t value : s_window) sum += value;
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.max_us = std::max(elapsed, s_stats.max_us);
    s_stats.inference_us += elapsed; ++s_stats.invokes;
    s_stats.probability = sum / WAKE_PROBABILITY_WINDOW;
    s_stats.peak = std::max(s_stats.probability, s_stats.peak);
    portEXIT_CRITICAL(&s_stats_lock);
    // Device calibration: ordinary speech peaked at 73/255; Tessa misses at 252.
    // Output scale is 1/256. Keep five-frame smoothing at an explicit 97% cutoff.
    return wake_should_trigger(s_features, sum);
}
bool wake_model_feed(const int16_t *pcm, size_t samples) {
    bool detected = false;
    while (s_interpreter && s_healthy && samples) {
        size_t used = 0;
        FrontendOutput features = FrontendProcessSamples(&s_frontend, pcm, samples, &used);
        if (!used) break;
        pcm += used; samples -= used;
        if (features.size == 40) detected = features_feed(features) || detected;
    }
    return detected;
}
uint32_t wake_model_max_us() { return statistics_snapshot().max_us; }
bool wake_model_healthy() { return s_healthy; }
uint8_t wake_model_probability() { return statistics_snapshot().probability; }

uint8_t wake_model_peak_probability() { return statistics_snapshot().peak; }
uint32_t wake_model_average_us() { Statistics stats = statistics_snapshot(); return stats.invokes ? stats.inference_us / stats.invokes : 0; }
