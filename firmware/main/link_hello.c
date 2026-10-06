#include "link_hello.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "devkey.h"
#include "settings.h"
#include "version.h"

bool link_is_welcome(const char *text, size_t len) {
    cJSON *j = cJSON_ParseWithLength(text, len);
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "t"));
    const char *session = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "session"));
    bool ok = t && !strcmp(t, "welcome") && session && session[0];
    cJSON_Delete(j);
    return ok;
}

static bool add_hello_identity(cJSON *j) {
    return devkey_public()[0] && cJSON_AddStringToObject(j, "t", "hello") && cJSON_AddNumberToObject(j, "v", 5) &&
        cJSON_AddStringToObject(j, "device", g_device_id) && cJSON_AddStringToObject(j, "key", devkey_public()) &&
        cJSON_AddStringToObject(j, "fw", KUBIK_FW_VERSION);
}

static bool add_hello_settings(cJSON *j, int volume) {
    return cJSON_AddStringToObject(j, "name", g_settings.name) && cJSON_AddStringToObject(j, "server", g_settings.server_url) &&
        cJSON_AddNumberToObject(j, "volume", volume);
}

bool link_make_hello(char *buf, size_t cap) {
    // Made on first use, not at boot: by now Wi-Fi is up and the RNG is truly random.
    if (!devkey_init()) return false;
    cJSON *j = cJSON_CreateObject();
    if (!j) return false;
    int volume = g_settings.volume < 0 ? 0 : g_settings.volume > 100 ? 100 : g_settings.volume;
    bool ok = add_hello_identity(j) && add_hello_settings(j, volume) && cJSON_PrintPreallocated(j, buf, (int)cap, false);
    cJSON_Delete(j);
    return ok;
}

bool link_make_auth(const char *nonce, const char *bind, char *buf, size_t cap) {
    char sig[112];
    bool ok = devkey_sign_challenge(nonce, bind, sig, sizeof sig) &&
        snprintf(buf, cap, "{\"t\":\"auth\",\"sig\":\"%s\"}", sig) < (int)cap;
    memset(sig, 0, sizeof sig);
    return ok;
}
