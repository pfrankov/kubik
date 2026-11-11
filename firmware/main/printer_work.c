#include "printer_work.h"
#include "printer_job.h"
#include "printer_guard.h"
#include "printer_platform.h"
#include "printer_result.h"
#include "peripage_ble.h"

#include <stdatomic.h>
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
    portEXIT_CRITICAL(&lock);
}

static printer_job_t *pending_job(void) {
    portENTER_CRITICAL(&lock);
    printer_job_t *job = pending;
    portEXIT_CRITICAL(&lock);
    return job;
}

static printer_job_t *make_job(const printer_input_t *input, uint32_t session, int64_t now) {
    size_t bytes = input && input->printing ? strlen(input->document.text) + 1 : 0;
    printer_job_t *job = calloc(1, sizeof *job + bytes);
    if (!job) return NULL;
    job->session = session; job->automatic = !input; job->printing = input && input->printing;
    job->deadline_ms = now + (job->printing ? PRINT_WORK_MS : STATUS_WORK_MS);
    atomic_init(&job->cancelled, false);
    if (input) memcpy(job->rid, input->rid, sizeof job->rid);
    if (bytes) {
        memcpy(job->text, input->document.text, bytes);
        job->document = input->document; job->document.text = job->text;
    }
    return job;
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
    free(candidate);
    return result;
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
    if (found) { automatic_found = true; return; }
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
    free(done);
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
    free(job);
    return response;
}

static void observe_session(const printer_app_state_t *state, int64_t now) {
    if (observed_session == state->session) return;
    observed_session = state->session;
    automatic_attempts = 0; automatic_found = false;
    automatic_due = now < automatic_earliest ? automatic_earliest : now;
}

static void automatic_job(const printer_app_state_t *state, int64_t now) {
    if (pending_job() || active || automatic_found || !state->automatic_allowed ||
        !state->online || !state->session || now < automatic_due || !printer_guard_background_allowed()) return;
    printer_job_t *job = make_job(NULL, state->session, now);
    if (!job) { automatic_due = now + 300000; return; }
    portENTER_CRITICAL(&lock);
    if (!pending && !active) { pending = job; job = NULL; }
    portEXIT_CRITICAL(&lock);
    free(job);
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
    free(failed);
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
