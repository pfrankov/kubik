#pragma once
#include <stdbool.h>
#include "muse_vm.h"
#ifdef __cplusplus
extern "C" {
#endif
const char *muse_noise_endpoint(void);
bool muse_noise_start(const muse_vm_t *vm);
void muse_noise_tick(void);
void muse_noise_stop(void);
bool muse_noise_ready(void);
bool muse_noise_failed(void);
#ifdef __cplusplus
}
#endif
