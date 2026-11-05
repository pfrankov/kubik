#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "printer_protocol.h"
#include "link_hello.h"
#include "settings.h"

#define ESP_LOGW(...) ((void)0)
static pthread_mutex_t route_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t session = 7;
static unsigned attempts, dispatched;
static bool rx_active, send_ok = true;
static char sent[32][144];
static const char RID[] = "0123456789abcdef0123456789abcdef";
uint32_t link_session(void) { return session; }
bool link_send_json_in_session(const char *json, uint32_t expected) {
    assert(!rx_active && expected == session);
    assert(!pthread_mutex_trylock(&route_lock));
    assert(attempts < 32);
    snprintf(sent[attempts++], sizeof sent[0], "%s", json);
    pthread_mutex_unlock(&route_lock);
    return send_ok;
}
static void dispatch_json(cJSON *request, const char *type) {
    (void)request; assert(strcmp(type, "printer")); dispatched++;
}
static bool agent_protocol_has_nul_escape(const char *json, size_t len) {
    (void)len; return strstr(json, "\\u0000") != NULL;
}

/* PRODUCTION_ON_JSON */

static int64_t now_ms(void) { return 0; }
static bool tick_power_off(int64_t now) { (void)now; return true; }
static void tick_ui_timeouts(int64_t now) { (void)now; }
static void app_guide_tick(void) {}
static void tick_network_power(int64_t now) { (void)now; }
static bool tick_recovery(void) { return false; }
static void status_tick(void) {}
static void tick_pickup_sound(int64_t now) { (void)now; }
static void tick_connection(int64_t now) { (void)now; }
static void tick_playback(int64_t now) { (void)now; }
static void tick_conversation(int64_t now) { (void)now; }
static void tick_sleep(int64_t now) { (void)now; }

/* PRODUCTION_APP_TICK */

static void receive_bytes(const char *raw, size_t length) {
    unsigned before = attempts;
    assert(!pthread_mutex_lock(&route_lock)); rx_active = true;
    on_json(raw, length);
    rx_active = false; pthread_mutex_unlock(&route_lock);
    assert(attempts == before); // RX must never recursively acquire its route mutex.
}
static void receive(const char *raw) { receive_bytes(raw, strlen(raw)); }
static void assert_reply(unsigned index, const char *rid, bool invalid) {
    assert(index < attempts);
    cJSON *reply = cJSON_Parse(sent[index]); assert(reply);
    assert(cJSON_GetArraySize(reply) == (invalid ? 4 : 3));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "t")), "printer_result"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "rid")), rid));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "status")), invalid ? "error" : "not_configured"));
    if (invalid) assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "code")), "invalid_request"));
    assert(!cJSON_GetObjectItem(reply, "text")); cJSON_Delete(reply);
}
static void result(const char *raw, bool invalid) {
    unsigned before = attempts; receive(raw); app_tick();
    assert(attempts == before + 1); assert_reply(before, RID, invalid);
    app_tick(); assert(attempts == before + 1);
    attempts = 0;
}
static void ignored(const char *raw) {
    unsigned before = attempts; receive(raw); app_tick(); assert(attempts == before);
}
static void text_result(const char *text, bool invalid) {
    cJSON *request = cJSON_CreateObject(); assert(request);
    cJSON_AddStringToObject(request, "t", "printer"); cJSON_AddStringToObject(request, "rid", RID);
    cJSON_AddStringToObject(request, "action", "print"); cJSON_AddStringToObject(request, "text", text);
    char *raw = cJSON_PrintUnformatted(request); assert(raw); cJSON_Delete(request);
    result(raw, invalid); free(raw);
}

static void valid_and_boundaries(void) {
    result("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"status\"}", false);
    text_result("Привет, Кубик!\nMilk\t2", false);
    text_result("literal \\u0000 is printable", false);
    char ascii[PRINTER_TEXT_MAX_BYTES + 2]; memset(ascii, 'x', sizeof ascii); ascii[1024] = 0;
    text_result(ascii, false); ascii[1024] = 'x'; ascii[1025] = 0; text_result(ascii, true);
    char cyrillic[1027];
    for (int i = 0; i < 513; i++) { cyrillic[2*i] = (char)0xd0; cyrillic[2*i+1] = (char)0xaf; }
    cyrillic[1024] = 0; text_result(cyrillic, false);
    cyrillic[1024] = (char)0xd0; cyrillic[1025] = (char)0xaf; cyrillic[1026] = 0; text_result(cyrillic, true);
    char emoji[1029];
    for (int i = 0; i < 257; i++) memcpy(emoji + 4*i, "\xf0\x9f\x98\x80", 4);
    emoji[1024] = 0; text_result(emoji, false);
    memcpy(emoji + 1024, "\xf0\x9f\x98\x80", 5); text_result(emoji, true);
    puts("printer: deferred honest not_configured, printable Unicode and byte bounds passed");
}

static void invalid_text(void) {
    const char *const values[] = {"", " \n\t", "\xc2\xa0\xe3\x80\x80", "\xef\xbb\xbf", "x\r", "x\x01", "x\x1b@",
        "x\x7f", "x\xc2\x80", "x\xc0\xaf", "x\xed\xa0\x80", "x\xf4\x90\x80\x80", "x\xe0\x80\xaf",
        "x\x80", "x\xe2\x82"};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) text_result(values[i], true);
    result("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"print\",\"text\":\"safe\\u0000suffix\"}", true);
    result("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"print\",\"text\":\"raw\nnewline\"}", true);
    result("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"status\"}{}", true);
    result("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"status\"} \n\t", false);
    result("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\\u0000suffix\",\"action\":\"status\"}", true);
    result("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"status\\u0000suffix\"}", true);
    char raw[] = "{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"status\"}\0trailing";
    receive_bytes(raw, sizeof raw - 1); app_tick(); assert_reply(0, RID, true); attempts = 0;
    char oversized[4098]; memset(oversized, ' ', sizeof oversized);
    const char *status = "{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"status\"}";
    memcpy(oversized, status, strlen(status)); oversized[sizeof oversized - 1] = 0;
    result(oversized, true);
    puts("printer: raw NUL, Unicode/control injection and malformed UTF-8 refused");
}

static void invalid_fields(void) {
    const char *const fields[] = {"\"action\":\"other\"", "\"action\":2", "\"action\":\"print\"",
        "\"action\":\"print\",\"text\":null", "\"action\":\"print\",\"text\":7",
        "\"action\":\"status\",\"text\":\"\"", "\"action\":\"status\",\"copies\":2",
        "\"action\":\"status\",\"action\":\"print\"", "\"action\":\"status\",\"T\":\"printer\"",
        "\"action\":\"status\",\"rid\":\"0123456789abcdef0123456789abcdef\""};
    char raw[512];
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        snprintf(raw, sizeof raw, "{\"t\":\"printer\",\"rid\":\"%s\",%s}", RID, fields[i]); result(raw, true);
    }
    ignored("{\"t\":\"printer\",\"action\":\"status\"}");
    ignored("{\"t\":\"printer\",\"rid\":\"0123456789ABCDEF0123456789abcdef\",\"action\":\"status\"}");
    ignored("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdeg\",\"action\":\"status\"}");
    ignored("{\"t\":\"printer\",\"rid\":\"short\",\"action\":\"status\"}");
    ignored("{\"t\":\"printer\",\"rid\":4,\"action\":\"status\"}");
    ignored("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef0\",\"action\":\"status\"}");
    ignored("not JSON"); ignored("[]");
    unsigned before = dispatched; ignored("{\"t\":\"welcome\",\"session\":\"unchanged\"}"); assert(dispatched == before + 1);
    session = 0; ignored("{\"t\":\"printer\",\"rid\":\"0123456789abcdef0123456789abcdef\",\"action\":\"status\"}"); session = 7;
    puts("printer: strict fields/IDs, zero session and ordinary JSON boundary passed");
}

static void outbox_and_sessions(void) {
    char raw[160], rid[33];
    for (unsigned i = 0; i < 1000; i++) {
        snprintf(rid, sizeof rid, "%032x", i);
        snprintf(raw, sizeof raw, "{\"t\":\"printer\",\"rid\":\"%s\",\"action\":\"status\"}", rid); receive(raw);
    }
    assert(!attempts);
    for (unsigned i = 0; i < PRINTER_REPLY_SLOTS; i++) {
        app_tick(); assert(attempts == i + 1); snprintf(rid, sizeof rid, "%032x", i); assert_reply(i, rid, false);
    }
    for (unsigned i = 0; i < 10; i++) app_tick();
    assert(attempts == PRINTER_REPLY_SLOTS); attempts = 0;
    snprintf(raw, sizeof raw, "{\"t\":\"printer\",\"rid\":\"%s\",\"action\":\"status\"}", RID);
    for (unsigned i = 0; i < 20; i++) receive(raw);
    app_tick(); assert(attempts == 1); app_tick(); assert(attempts == 1); attempts = 0;
    receive(raw); session++; app_tick(); assert(!attempts); // Stale slot is consumed without sending.
    result(raw, false); // New session immediately reuses capacity.
    receive(raw); session++; receive(raw); app_tick(); assert(attempts == 1); app_tick(); assert(attempts == 1); attempts = 0;
    send_ok = false; receive(raw); app_tick(); assert(attempts == 1); app_tick(); assert(attempts == 1); attempts = 0; send_ok = true;
    result(raw, false);
    puts("printer: fixed outbox/flood/duplicate bounds, stale-session release and send failure no-retry passed");
}

settings_t g_settings;
char g_device_id[20] = "kubik-782900";
static bool key_ready = true;
bool devkey_init(void) { return key_ready; }
const char *devkey_public(void) { return key_ready ? "synthetic-public-key" : ""; }
bool devkey_sign_challenge(const char *nonce, const char *bind, char *out, size_t cap) {
    (void)nonce; (void)bind; snprintf(out, cap, "synthetic-signature"); return true;
}
static void hello_descriptor(void) {
    snprintf(g_settings.name, sizeof g_settings.name, "My Kubik");
    snprintf(g_settings.server_url, sizeof g_settings.server_url, "kubik://example.test:18794");
    g_settings.volume = 42; g_settings.ui_volume = 57; g_settings.brightness = 150; g_settings.guide_done = true;
    settings_t before = g_settings; char hello[512];
    assert(link_make_hello(hello, sizeof hello)); cJSON *j = cJSON_Parse(hello); assert(j);
    assert(cJSON_GetObjectItem(j, "printer")->valueint == PRINTER_PROTOCOL_VERSION);
    assert(cJSON_GetObjectItem(j, "v")->valueint == 5 && cJSON_GetObjectItem(j, "volume")->valueint == 42);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "device")), g_device_id));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "key")), devkey_public()));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "server")), g_settings.server_url));
    assert(!memcmp(&before, &g_settings, sizeof before)); cJSON_Delete(j);
    key_ready = false; assert(!link_make_hello(hello, sizeof hello)); key_ready = true;
    assert(!link_make_hello(hello, 16));
    puts("printer: hello advertises protocol1 while preserving v5 identity and every saved setting");
}

int main(void) {
    valid_and_boundaries(); invalid_text(); invalid_fields(); outbox_and_sessions(); hello_descriptor();
    puts("printer protocol: all production-module and application RX/tick checks passed");
}
