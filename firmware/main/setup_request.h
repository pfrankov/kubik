#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct { bool muse; char sdk_token[96]; } setup_agent_choice_t;
const char *setup_validate_credentials(const char *ssid, const char *password);
const char *setup_agent_choice(const cJSON *request, setup_agent_choice_t *choice);
esp_err_t setup_agent_save(const setup_agent_choice_t *choice);
cJSON *setup_read_request(httpd_req_t *request);
