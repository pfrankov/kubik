#include "printer_work.h"
#include "printer_job.h"
#include "printer_guard.h"
#include "printer_platform.h"
#include "printer_result.h"
#include "printer_status.h"
#include "printer_status_internal.h"
#include "printer_emoji_parse.h"
#include "peripage_ble.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "link.h"
#include "esp_log.h"

#define STATUS_WORK_MS 8000
#define PRINT_WORK_MS 78000
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static printer_job_t *pending, *active;
static char *completed;
static bool found;
static uint32_t observed_session;
static unsigned automatic_attempts;
static int64_t automatic_due;
static int64_t automatic_earliest;
static atomic_bool automatic_found;
static printer_diagnostics_t diagnostics;
static printer_status_t status = {.battery_pct=-1, .charging=-1, .stale=true};
static atomic_bool status_visible;
static uint32_t refresh_session;
static int64_t refresh_due;

static bool refresh_allowed_locked(int64_t now) {
    return !pending && !active && !refresh_session && diagnostics.state.automatic_allowed &&
        diagnostics.state.online && diagnostics.state.session == link_session() && now >= refresh_due &&
        (status.state == PRINTER_STATUS_UNCHECKED || now-status.last_checked_ms >= 60000);
}

void printer_status_snapshot(printer_status_t *out) {
    int64_t now = printer_platform_now_ms();
    portENTER_CRITICAL(&lock);
    *out = status;
    out->busy = pending || active;
    out->can_refresh = refresh_allowed_locked(now);
    portEXIT_CRITICAL(&lock);
    out->stale = out->state != PRINTER_STATUS_VERIFIED || now-out->last_verified_ms >= 60000;
}

void printer_status_set_visible(bool visible) { atomic_store(&status_visible, visible); }

bool printer_status_refresh(void) {
    int64_t now = printer_platform_now_ms();
    portENTER_CRITICAL(&lock);
    bool allowed = refresh_allowed_locked(now);
    if (allowed) refresh_session = diagnostics.state.session;
    portEXIT_CRITICAL(&lock);
    return allowed;
}

void printer_status_observe(const peripage_info_t *info, bool verified, const char *error) {
    int64_t now = printer_platform_now_ms();
    portENTER_CRITICAL(&lock);
    if (info) {
        memcpy(status.name, info->name, sizeof status.name);
        status.battery_pct = info->battery_pct;
        status.charging = info->charging;
    }
    status.state = verified ? PRINTER_STATUS_VERIFIED : PRINTER_STATUS_UNAVAILABLE;
    status.last_checked_ms = now;
    if (verified) status.last_verified_ms = now;
    snprintf(status.error, sizeof status.error, "%s", error ? error : "");
    portEXIT_CRITICAL(&lock);
}

void printer_work_diagnostics(printer_diagnostics_t *snapshot) {
    portENTER_CRITICAL(&lock);
    *snapshot = diagnostics;
    portEXIT_CRITICAL(&lock);
}

static void remember(const printer_app_state_t *state, int64_t now) {
    printer_diagnostics_t snapshot = {.state=*state, .observed_session=observed_session,
        .attempts=automatic_attempts, .due_in_ms=automatic_due > now ? automatic_due-now : 0,
        .busy=printer_work_busy(), .discovered=automatic_found,
        .guard_allowed=printer_guard_background_allowed()};
    portENTER_CRITICAL(&lock);
    diagnostics = snapshot;
    refresh_due = automatic_due;
    portEXIT_CRITICAL(&lock);
}

static printer_job_t *pending_job(void) {
    portENTER_CRITICAL(&lock);
    printer_job_t *job = pending;
    portEXIT_CRITICAL(&lock);
    return job;
}

static bool copy_job_input(printer_job_t *job, const printer_input_t *input, size_t bytes) {
    if (!input) return true;
    memcpy(job->rid, input->rid, sizeof job->rid);
    if (input->image) {
        job->image = true; job->image_width = input->image_width; job->image_height = input->image_height;
        job->stream = printer_stream_create(job->rid, job->session, input->image_height);
        return job->stream != NULL;
    }
    if (bytes) {
        memcpy(job->text, input->document.text, bytes);
        job->document = input->document; job->document.text = job->text;
        printer_emoji_glyph_t *glyphs=(printer_emoji_glyph_t *)(job->text+bytes);
        if (input->emoji_count && !printer_emoji_parse_copy(input->emoji,glyphs,input->emoji_count)) {
            return false;
        }
        job->document.emojis=input->emoji_count ? glyphs : NULL;
        job->document.emoji_count=input->emoji_count;
    }
    return true;
}

static printer_job_t *make_job(const printer_input_t *input, uint32_t session, int64_t now) {
    size_t bytes = input && input->printing && !input->image ? strlen(input->document.text) + 1 : 0;
    size_t glyph_bytes=input ? input->emoji_count*sizeof(printer_emoji_glyph_t) : 0;
    printer_job_t *job = calloc(1, sizeof *job + bytes + glyph_bytes);
    if (!job) return NULL;
    job->session = session; job->automatic = !input; job->printing = input && input->printing;
    job->deadline_ms = now + (job->printing ? PRINT_WORK_MS : STATUS_WORK_MS);
    atomic_init(&job->cancelled, false);
    if (!copy_job_input(job, input, bytes)) { free(job); return NULL; }
    return job;
}

static void free_job(printer_job_t *job) {
    if (!job) return;
    printer_stream_destroy(job->stream);
    free(job);
}

static printer_admission_t admission(const char *rid) {
    printer_job_t *previous = active ? active : pending;
    if (!previous) return PRINTER_ADMITTED;
    return !strcmp(previous->rid, rid) ? PRINTER_DUPLICATE : PRINTER_WORK_BUSY;
}

printer_admission_t printer_work_submit(const printer_input_t *input, uint32_t session) {
    if (!input || !input->rid || !session) return PRINTER_WORK_FAILED;
    portENTER_CRITICAL(&lock);
    printer_admission_t result = admission(input->rid);
    portEXIT_CRITICAL(&lock);
    if (result != PRINTER_ADMITTED) return result;
    printer_job_t *candidate = make_job(input, session, printer_platform_now_ms());
    if (!candidate) return PRINTER_WORK_FAILED;
    portENTER_CRITICAL(&lock);
    result = admission(input->rid);
    if (result == PRINTER_ADMITTED) { pending = candidate; candidate = NULL; }
    portEXIT_CRITICAL(&lock);
    free_job(candidate);
    return result;
}

bool printer_work_image_credit(printer_stream_credit_t *credit) {
    portENTER_CRITICAL(&lock);
    bool ready = active && active->image && printer_stream_credit(active->stream, credit);
    portEXIT_CRITICAL(&lock);
    return ready;
}
void printer_work_image_receive(uint32_t session, const uint8_t *data, size_t length) {
    portENTER_CRITICAL(&lock);
    printer_job_t *job = active ? active : pending;
    if (job && job->image) printer_stream_receive(job->stream, session, data, length);
    portEXIT_CRITICAL(&lock);
}
void printer_work_image_abort(uint32_t session, const char *rid) {
    if (!rid) return;
    portENTER_CRITICAL(&lock);
    printer_job_t *job = active ? active : pending;
    if (job && job->image && job->session == session && !strcmp(job->rid, rid)) {
        atomic_store(&job->cancelled, true);
        printer_stream_abort(job->stream);
    }
    portEXIT_CRITICAL(&lock);
}

bool printer_work_busy(void) {
    portENTER_CRITICAL(&lock);
    bool busy = pending || active;
    portEXIT_CRITICAL(&lock);
    return busy;
}

bool printer_work_has_id(const char *rid) {
    portENTER_CRITICAL(&lock);
    printer_job_t *job = active ? active : pending;
    bool duplicate = job && !strcmp(job->rid, rid);
    portEXIT_CRITICAL(&lock);
    return duplicate;
}

bool printer_work_discovered(void) { return atomic_load(&automatic_found); }

static void execute(void *context) {
    printer_job_t *job = context;
    completed = printer_job_run(job, &found);
    ESP_LOGI("printer", "%s operation verified=%d session=%u", job->automatic ? "automatic" : "requested",
        found, (unsigned)job->session);
}

static void retry_after(const printer_job_t *job, int64_t now) {
    if (job->session != observed_session) return;
    if (found) {
        automatic_found = true; automatic_attempts = 0; automatic_due = now + 60000;
        return;
    }
    if (!job->automatic) return;
    unsigned delay = automatic_attempts == 0 ? 30000 : automatic_attempts == 1 ? 60000 : 300000;
    if (automatic_attempts < 3) automatic_attempts++;
    automatic_due = now + delay;
}

static bool collect(char **reply, uint32_t *session, int64_t now) {
    if (!active || !printer_platform_finished()) return false;
    printer_platform_reap();
    retry_after(active, now);
    *session = active->session; *reply = completed; completed = NULL;
    bool response = !active->automatic;
    portENTER_CRITICAL(&lock);
    printer_job_t *done = active; active = NULL;
    portEXIT_CRITICAL(&lock);
    free_job(done);
    printer_platform_release();
    return response;
}

static bool expire_pending(char **reply, uint32_t *session, const printer_app_state_t *state, int64_t now) {
    printer_job_t *queued = pending_job();
    if (!queued) return false;
    bool cancelled = atomic_load(&queued->cancelled);
    if (!cancelled && queued->session == state->session && state->online && now < queued->deadline_ms) return false;
    portENTER_CRITICAL(&lock);
    printer_job_t *job = pending; pending = NULL;
    portEXIT_CRITICAL(&lock);
    *session = job->session;
    bool response = !job->automatic && state->session == job->session;
    if (response) *reply = printer_result_make(job, "printer_busy", NULL, NULL);
    if (job->automatic) automatic_due = now + 30000;
    free_job(job);
    return response;
}

static void observe_session(const printer_app_state_t *state, int64_t now) {
    if (observed_session == state->session) return;
    observed_session = state->session;
    automatic_attempts = 0; automatic_found = false;
    automatic_due = now < automatic_earliest ? automatic_earliest : now;
    portENTER_CRITICAL(&lock);
    refresh_session = 0;
    portEXIT_CRITICAL(&lock);
}

static bool take_refresh(const printer_app_state_t *state) {
    portENTER_CRITICAL(&lock);
    uint32_t requested = refresh_session; refresh_session = 0;
    portEXIT_CRITICAL(&lock);
    return requested && requested == state->session;
}

static bool status_check_wanted(bool requested, int64_t now) {
    if (requested || !automatic_found) return true;
    if (!atomic_load(&status_visible)) return false;
    printer_status_t snapshot; printer_status_snapshot(&snapshot);
    return snapshot.stale || now-snapshot.last_checked_ms >= 60000;
}

static void automatic_job(const printer_app_state_t *state, int64_t now) {
    bool requested = take_refresh(state);
    if (pending_job() || active || !state->automatic_allowed ||
        !state->online || !state->session || now < automatic_due) return;
    if (!requested && !printer_guard_background_allowed()) return;
    if (!status_check_wanted(requested, now)) return;
    printer_job_t *job = make_job(NULL, state->session, now);
    if (!job) { automatic_due = now + 300000; return; }
    portENTER_CRITICAL(&lock);
    if (!pending && !active) { pending = job; job = NULL; }
    portEXIT_CRITICAL(&lock);
    free_job(job);
}

static bool launch(const printer_app_state_t *state, char **reply, uint32_t *session) {
    if (!pending_job() || active || !state->can_start) return false;
    portENTER_CRITICAL(&lock);
    active = pending; pending = NULL;
    portEXIT_CRITICAL(&lock);
    completed = NULL; found = false;
    if (active->automatic) automatic_earliest = printer_platform_now_ms() + 30000;
    if (printer_platform_launch(execute, active)) return false;
    *session = active->session;
    bool response = !active->automatic;
    if (response) *reply = printer_result_make(active, "printer_failed", NULL, NULL);
    automatic_due = printer_platform_now_ms() + 300000;
    portENTER_CRITICAL(&lock);
    printer_job_t *failed = active; active = NULL;
    portEXIT_CRITICAL(&lock);
    free_job(failed);
    return response;
}

bool printer_work_poll(char **reply, uint32_t *session) {
    *reply = NULL; *session = 0;
    printer_platform_boot();
    int64_t now = printer_platform_now_ms();
    printer_app_state_t state; printer_app_state(&state);
    observe_session(&state, now);
    bool response = collect(reply, session, now);
    if (!response) response = expire_pending(reply, session, &state, now);
    if (!response && state.enabled) {
        automatic_job(&state, now);
        response = launch(&state, reply, session);
    }
    remember(&state, now);
    return response;
}

static bool automatic_busy(void) {
    portENTER_CRITICAL(&lock);
    printer_job_t *job = active ? active : pending;
    bool busy = job && job->automatic;
    portEXIT_CRITICAL(&lock);
    return busy;
}

bool printer_work_before_voice(void) {
    portENTER_CRITICAL(&lock);
    printer_job_t *job = active ? active : pending;
    bool cancel = job && job->automatic;
    if (cancel) atomic_store(&job->cancelled, true);
    portEXIT_CRITICAL(&lock);
    if (!cancel) return true;
    peripage_cancel();
    int64_t end = printer_platform_now_ms() + 2000;
    while (printer_platform_now_ms() < end) {
        if (!automatic_busy()) return true;
        char *reply; uint32_t session;
        printer_work_poll(&reply, &session); free(reply);
        printer_platform_wait_tick();
    }
    return !automatic_busy();
}
