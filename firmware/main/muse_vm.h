#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct { char id[128]; char token[2048]; } muse_vm_t;
esp_err_t muse_vm_lookup(muse_vm_t *out);
void muse_device_identity(char *node, unsigned node_cap, char *device, unsigned device_cap);
