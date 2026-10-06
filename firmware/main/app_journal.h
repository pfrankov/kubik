#pragma once
#include "app.h"
#include "app_mailbox.h"
#include "cJSON.h"
void app_journal_event(const app_ev_t *event);
// Caller owns the face lock. Text is a bounded excerpt, never a raw wire payload.
void app_journal_text(const char *text, bool notification);
void app_journal_info(cJSON *info);
