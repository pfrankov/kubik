#include "muse_backend.h"
#include "muse_noise.h"
#include "muse_pair.h"
#include "muse_store.h"
#include "muse_options.h"
#include "muse_control.h"
#include "muse_chat.h"
#include "muse_link.h"
#include "app_wake.h"
#include "app.h"
#include "wifi.h"
#include "wake_resample.h"
#include "ima_adpcm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// All SDK turn and Noise calls share one worker. The audio callback only
// queues bounded PCM; it never parses JSON, writes TLS or allocates memory.
typedef enum { BEGIN, END, CANCEL, PCM, OPTIONS } command_kind_t;
typedef struct {
    command_kind_t kind;
    uint8_t turn;
    uint16_t size;
    uint32_t session;
    union { uint8_t ima[IMA_HEADER_BYTES + LINK_MIC_PCM_MAX / 4]; char json[512]; } data;
} command_t;
static QueueHandle_t queue;
static bool selected, online, recording;
static atomic_bool allowed = true, overflow, transport_active;
static SemaphoreHandle_t lifecycle_lock;
static unsigned current_turn;
static link_handlers_t handlers;
static void (*route_ready)(bool);
static wake_resample_t resampler;
static char *previous_text;
extern const char *muse_hatch_turn_text(void);

bool muse_backend_selected(void) { return selected; }
void muse_backend_allow(bool value) {
    if (lifecycle_lock) xSemaphoreTake(lifecycle_lock, portMAX_DELAY);
    atomic_store(&allowed, value);
    if (lifecycle_lock) xSemaphoreGive(lifecycle_lock);
}
bool muse_backend_wifi_stopped(void) { return !atomic_load(&transport_active); }
bool muse_link_hatch_linked(void) { return selected && !muse_pair_active(); }
bool muse_wifi_connected(void) { return wifi_sta_connected(); }

static void emit(const char *message) {
    if (handlers.on_json) handlers.on_json(message, strlen(message));
}
static void emit_object(cJSON *object) {
    if (!object) return;
    char *message = cJSON_PrintUnformatted(object);
    if (message) { emit(message); free(message); }
    cJSON_Delete(object);
}
static void publish_text(void) {
    const char *text = muse_hatch_turn_text();
    if (!text[0] || !strcmp(text, previous_text)) return;
    snprintf(previous_text, 1024, "%s", text);
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "t", "text"); cJSON_AddStringToObject(object, "text", text);
    emit_object(object);
}
static void set_online(bool value) {
    if (online == value) return;
    online = value; route_ready(value);
    if (value) {
        emit("{\"t\":\"welcome\",\"progress\":true}");
        emit("{\"t\":\"capabilities\",\"mode\":\"classic\",\"stt\":{\"available\":true},\"tts\":{\"available\":false}}");
    }
}
static void notification(const char *text) {
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "t", "text"); cJSON_AddStringToObject(object, "kind", "notify");
    cJSON_AddStringToObject(object, "text", text); emit_object(object);
}
static bool enqueue(command_t *command) {
    if (!queue || xQueueSend(queue, command, 0) != pdTRUE) {
        atomic_store(&overflow, true); return false;
    }
    return true;
}
bool muse_backend_json(const char *message, uint32_t session) {
    if (!selected || !queue) return false;
    cJSON *object = cJSON_Parse(message);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "t"));
    command_t command = {.session=session}; bool result = true;
    if (type && !strcmp(type, "ptt")) {
        command.kind = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "on")) ? BEGIN : END;
        cJSON *turn = cJSON_GetObjectItemCaseSensitive(object, "turn");
        command.turn = cJSON_IsNumber(turn) ? turn->valueint : 0;
        result = enqueue(&command);
    } else if (type && !strcmp(type, "cancel")) { command.kind = CANCEL; result = enqueue(&command); }
    else if (type && (!strcmp(type, "agent_options") || !strcmp(type, "agent_model"))) {
        command.kind = OPTIONS;
        size_t size = strlen(message);
        if (size >= sizeof command.data.json) result = false;
        else { memcpy(command.data.json, message, size + 1); result = enqueue(&command); }
    }
    cJSON_Delete(object); return result;
}
bool muse_backend_pcm(uint8_t turn, const int16_t *pcm, size_t bytes, const uint8_t *ima, uint32_t session) {
    if (!pcm || !bytes || bytes > LINK_MIC_PCM_MAX || bytes % 12 || !queue) return false;
    command_t command = {.kind=PCM,.turn=turn,.size=bytes,.session=session};
    if (ima) memcpy(command.data.ima, ima, IMA_HEADER_BYTES + bytes / 4);
    else { ima_state_t encoder = {0}; ima_encode(&encoder, pcm, bytes / 2, command.data.ima); }
    // This runs on mic_delivery, whose compressed queue keeps DMA independent.
    if (xQueueSend(queue, &command, pdMS_TO_TICKS(1000)) == pdTRUE) return true;
    atomic_store(&overflow, true); return false;
}
static void handle_command(const command_t *command) {
    if (command->session != link_session()) return;
    switch (command->kind) {
    case BEGIN:
        if (!online) return;
        current_turn = command->turn; memset(&resampler, 0, sizeof resampler); previous_text[0] = 0;
        muse_hatch_turn_begin(); recording = true;
        break;
    case END:
        if (!recording || command->turn != current_turn) return;
        recording = false; muse_hatch_turn_end(); emit("{\"t\":\"state\",\"s\":\"thinking\"}");
        break;
    case CANCEL: recording = false; muse_hatch_turn_cancel(); emit("{\"t\":\"state\",\"s\":\"idle\"}"); break;
    case PCM:
        if (recording && command->turn == current_turn) {
            int16_t pcm[LINK_MIC_PCM_MAX / 2], mono[LINK_MIC_PCM_MAX / 3]; ima_state_t decoder;
            if (!ima_read_header(command->data.ima, &decoder)) return;
            ima_decode(&decoder, command->data.ima + IMA_HEADER_BYTES, command->size / 4, pcm);
            size_t count = wake_resample(&resampler, pcm, command->size / 2, mono);
            muse_hatch_turn_audio(mono, count);
        }
        break;
    case OPTIONS: {
        cJSON *request = cJSON_Parse(command->data.json);
        emit_object(muse_options_reply(request, online)); cJSON_Delete(request);
        break;
    }
    }
}
static void poll_turn(void) {
    char detail[80];
    for (unsigned i = 0; i < 8; i++) {
        muse_hatch_ev_t event = muse_hatch_turn_event(detail, sizeof detail);
        if (event == MUSE_HATCH_EV_NONE) break;
        if (event == MUSE_HATCH_EV_REPLY || event == MUSE_HATCH_EV_DONE) publish_text();
        if (event == MUSE_HATCH_EV_DONE) emit("{\"t\":\"state\",\"s\":\"idle\"}");
        else if (event == MUSE_HATCH_EV_ERROR) {
            recording = false; emit("{\"t\":\"error\",\"code\":\"agent_failed\"}");
            ESP_LOGW("muse", "turn failed");
        }
    }
    publish_text();
}
static void disconnect(void) {
    set_online(false); recording = false; muse_hatch_turn_cancel();
    muse_control_clear();
    muse_noise_stop(); xQueueReset(queue); atomic_store(&overflow, false);
    app_wake_transport_busy(false); atomic_store(&transport_active, false);
}
static bool connect_vm(void) {
    xSemaphoreTake(lifecycle_lock, portMAX_DELAY);
    bool start = atomic_load(&allowed);
    if (start) atomic_store(&transport_active, true);
    xSemaphoreGive(lifecycle_lock);
    if (!start) return false;
    app_wake_transport_busy(true);
    muse_vm_t *vm = calloc(1, sizeof *vm);
    bool ok = vm && muse_vm_lookup(vm) == ESP_OK && atomic_load(&allowed) && muse_noise_start(vm);
    if (vm) { muse_store_wipe(vm, sizeof *vm); free(vm); }
    if (!ok) { app_wake_transport_busy(false); atomic_store(&transport_active, false); }
    return ok;
}
static void network_step(bool *connecting, int64_t *next_attempt) {
    bool can_connect = atomic_load(&allowed) && wifi_sta_connected() && wifi_time_ready();
    if (atomic_exchange(&overflow, false)) {
        recording = false; muse_hatch_turn_cancel(); xQueueReset(queue);
        emit("{\"t\":\"error\",\"code\":\"stt_failed\"}");
    }
    if (!can_connect || muse_noise_failed() || muse_control_failed()) {
        if (*connecting || online) disconnect();
        *connecting = false; *next_attempt = esp_timer_get_time() + 5000000;
    }
    if (can_connect && !*connecting && esp_timer_get_time() >= *next_attempt) {
        *connecting = connect_vm(); *next_attempt = esp_timer_get_time() + 20000000;
    }
}
static command_t *worker_buffers(void) {
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        command_t *command = malloc(sizeof *command);
        previous_text = calloc(1, 1024);
        if (command && previous_text && muse_hatch_start()) return command;
        free(previous_text); previous_text = NULL; free(command);
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGE("muse", "chat memory unavailable");
    xSemaphoreTake(g_face_mtx, portMAX_DELAY);
    face_card(&g_face, "Muse unavailable\nNot enough memory. Restart Kubik.");
    xSemaphoreGive(g_face_mtx); disp_wake();
    return NULL;
}
static void worker(void *argument) {
    (void)argument; command_t *command = worker_buffers();
    if (!command) { vTaskDelete(NULL); return; }
    int64_t next_attempt = 0; bool connecting = false;
    for (;;) {
        network_step(&connecting, &next_attempt);
        if (connecting) {
            muse_noise_tick();
            if (muse_noise_ready()) muse_control_start(notification);
            set_online(muse_control_ready());
            if (online) app_wake_transport_busy(false);
            for (unsigned i = 0; i < 4 && xQueueReceive(queue, command, 0) == pdTRUE; i++) handle_command(command);
            poll_turn();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void muse_backend_init(const link_handlers_t *application, void (*route)(bool)) {
    muse_state_t state = muse_store_state();
    selected = state != MUSE_OFF; handlers = *application; route_ready = route;
    if (!selected) return;
    if (state == MUSE_PAIRING) {
        app_wake_transport_busy(true);
        esp_err_t err = muse_pair_start();
        if (err != ESP_OK) ESP_LOGE("muse", "pairing could not start: %s", esp_err_to_name(err));
        return;
    }
    lifecycle_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(lifecycle_lock ? ESP_OK : ESP_ERR_NO_MEM);
    queue = xQueueCreate(8, sizeof(command_t));
    ESP_ERROR_CHECK(queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(worker, "muse", 7168, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
