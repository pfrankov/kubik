// SDK mocks for the real link state-machine harness.
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "cJSON.h"
#include "link.h"
#include "ima_adpcm.h"
#include "link_mic.h"
#include "link_usb.h"
#include "link_ws.h"
#include "version.h"
#include "devkey.h"
#include "link_discover.h"
#include "link_hello.h"
#include "link_poke.h"
#include "link_pin.h"
#define link_up() (strcmp(link_via(), "none") != 0)  // link_up() is not in the firmware: only the route name is
typedef int esp_err_t;
typedef const char *esp_event_base_t;
typedef unsigned TickType_t;
typedef struct { bool held; } Lock;
typedef Lock *SemaphoreHandle_t;
static void (*before_take)(SemaphoreHandle_t);
static Lock locks[3]; static int nlocks;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { assert(nlocks<3); return &locks[nlocks++]; }
static int xSemaphoreTake(SemaphoreHandle_t l,unsigned timeout) { (void)timeout; if(before_take)before_take(l); assert(l && !l->held); l->held=true; return 1; }
static int xPortInIsrContext(void) { return 0; }
#define taskSCHEDULER_RUNNING 2
static int xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }
static void xSemaphoreGive(SemaphoreHandle_t l) { assert(l->held); l->held=false; }
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffu
#define ESP_OK 0
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define hp_mark(phase) ((void)0)
static struct { char wifi_ssid[33], server_url[128], name[32]; int volume; } g_settings;
// devkey.h stub: fixed public key, "SIG" signatures, records what it signed.
static bool devkey_ok=true; static char signed_nonce[80], signed_bind[LINK_BIND_MAX]; static int signs;
bool devkey_init(void) { return devkey_ok; }
const char *devkey_public(void) { return devkey_ok?"BFakePublicKeyBase64==":""; }
bool devkey_sign_challenge(const char *nonce,const char *bind,char *sig,size_t cap) {
    snprintf(signed_nonce,sizeof signed_nonce,"%s",nonce); snprintf(signed_bind,sizeof signed_bind,"%s",bind);
    signs++; snprintf(sig,cap,"SIG"); return true;
}
// link_pin.h stub: the real verify callback is covered by tools/test-pin.py.
static char wifi_bind_seen[LINK_BIND_MAX]="0123abcd", usb_bind_text[LINK_BIND_MAX]; static uint32_t usb_bind_epoch;
static bool usb_bind_set, pin_mismatch; static int keeps_wifi, keeps_usb;
server_mode_t link_server_mode(const char *url) {
    if (!url[0]) return SERVER_LAN_DISCOVER;
    if (!strncmp(url,"kubik://",8)) return SERVER_LAN_FIXED;
    return strncmp(url,"wss://",6)?SERVER_INVALID:SERVER_CA;
}
esp_err_t link_pin_attach(void *conf) { (void)conf; return 0; }
void link_pin_begin_wifi(void) { }
void link_pin_usb_bind(uint32_t epoch,const char *bind,size_t len) {
    usb_bind_set=strncmp(bind,"bad",len)!=0; pin_mismatch=!usb_bind_set;
    memcpy(usb_bind_text,bind,len); usb_bind_text[len]=0; usb_bind_epoch=epoch;
}
bool link_pin_bind(bool usb,uint32_t epoch,char out[LINK_BIND_MAX]) {
    if (usb) { if(!usb_bind_set||epoch!=usb_bind_epoch)return false; strcpy(out,usb_bind_text); return true; }
    server_mode_t mode=link_server_mode(g_settings.server_url);
    if(mode==SERVER_INVALID)return false;
    strcpy(out,mode==SERVER_CA?"ca:gw.example.com":wifi_bind_seen); return true;
}
void link_pin_keep(bool usb,uint32_t epoch) { (void)epoch; if(usb)keeps_usb++; else keeps_wifi++; }
bool link_pin_take_mismatch(void) { bool m=pin_mismatch; pin_mismatch=false; return m; }
// link_discover.h stub: a LAN server is "found" at a fixed address.
static link_target_t target_result=TARGET_READY; static int target_stops;
link_target_t link_target(uint32_t now,char *uri,size_t cap) {
    (void)now; if(target_result==TARGET_READY)snprintf(uri,cap,"%s",g_settings.server_url[0]?g_settings.server_url:"wss://10.0.0.5:18790/kubik/v1");
    return target_result;
}
void link_target_stop(void) { target_stops++; }
static char g_device_id[20]="kubik-test";
static uint32_t time_ms;
static bool wake_transport_busy; static void app_wake_transport_busy(bool busy) { wake_transport_busy = busy; }
static bool wifi_connected=true, clock_ready=true;
static int64_t esp_timer_get_time(void) { return (int64_t)time_ms*1000; }
static void wifi_start_sta(void) { }
static bool wifi_sta_connected(void) { return wifi_connected; }
static bool wifi_time_ready(void) { return clock_ready; }
static bool poll_mode;
static unsigned poll_attempts;
static bool wifi_radio_started(void) { return true; }
bool wifi_polling(void) { return poll_mode; }
static bool wifi_link_attempt_allowed(void) { return !poll_mode || poll_attempts < 3; }
static void wifi_link_attempt_started(void) { if (poll_mode) poll_attempts++; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock"; }
static unsigned char usb_bytes[32768]; static size_t usb_size;
static unsigned char usb_input[8192]; static size_t usb_input_size, usb_input_pos;
static bool stop_usb_rx; static jmp_buf usb_rx_jmp;
static int usb_serial_jtag_write_bytes(const void *p,size_t n,unsigned timeout) { (void)timeout; assert(usb_size+n<=sizeof usb_bytes); memcpy(usb_bytes+usb_size,p,n); usb_size+=n; return n; }
static int usb_serial_jtag_read_bytes(void *p,size_t n,unsigned timeout) {
    (void)timeout;
    if (usb_input_pos < usb_input_size) {
        size_t count=usb_input_size-usb_input_pos;if(count>n)count=n;
        memcpy(p,usb_input+usb_input_pos,count);usb_input_pos+=count;return (int)count;
    }
    if (stop_usb_rx) longjmp(usb_rx_jmp,1);
    return 0;
}
typedef struct { int rx_buffer_size,tx_buffer_size; } usb_serial_jtag_driver_config_t;
static int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *c) { (void)c; return 0; }
static void usb_serial_jtag_vfs_use_driver(void) { }
static void esp_log_set_vprintf(int (*cb)(const char*,va_list)) { (void)cb; }
static int xTaskCreate(void (*fn)(void*),const char *name,int stack,void *arg,int pri,void *out) { (void)fn;(void)name;(void)stack;(void)arg;(void)pri;(void)out; return 1; }
static jmp_buf manager_jmp; static int manager_ticks, manager_limit;
static void (*tick_hook)(void);
static void vTaskDelay(unsigned ms) { time_ms+=ms; if(tick_hook)tick_hook(); if(++manager_ticks>=manager_limit)longjmp(manager_jmp,1); }
typedef enum { WEBSOCKET_ERROR_TYPE_NONE=0,WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT,WEBSOCKET_ERROR_TYPE_HANDSHAKE,WEBSOCKET_ERROR_TYPE_SERVER_CLOSE } esp_websocket_error_type_t;
typedef struct { esp_websocket_error_type_t error_type; int esp_ws_handshake_status_code; } esp_websocket_error_codes_t;
typedef struct { int op_code,payload_offset,payload_len,data_len; bool fin; const char *data_ptr; esp_websocket_error_codes_t error_handle; } esp_websocket_event_data_t;
enum { WEBSOCKET_EVENT_CONNECTED,WEBSOCKET_EVENT_ERROR,WEBSOCKET_EVENT_DISCONNECTED,WEBSOCKET_EVENT_CLOSED,WEBSOCKET_EVENT_FINISH,WEBSOCKET_EVENT_DATA,WEBSOCKET_EVENT_ANY };
typedef struct MockWS { void (*cb)(void*,esp_event_base_t,int32_t,void*); void *arg; } *esp_websocket_client_handle_t;
typedef struct { const char *uri; int buffer_size; bool disable_auto_reconnect; int network_timeout_ms,ping_interval_sec,pingpong_timeout_sec,task_stack,task_prio; esp_err_t (*crt_bundle_attach)(void *); } esp_websocket_client_config_t;
static esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return 0; }
static char last_uri[160]; static esp_err_t (*last_attach)(void *);
static int fail_init, fail_register, fail_start, allocations, destroys, starts, sends;
static atomic_bool s_wifi_allowed;
static bool auto_welcome=true, connect_on_start=true, stop_during_ws_start;
static char ws_last[LINK_JSON_MAX+1];
static bool fail_rx;
static size_t rx_last_bytes;
static void *rx_malloc(size_t n) { rx_last_bytes=n; return fail_rx ? NULL : malloc(n); }
static uint32_t attempts[64]; static int nattempts;
static esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *c) {
    assert(c->disable_auto_reconnect); assert(c->network_timeout_ms==(!strncmp(c->uri,"wss://",6)?15000:8000)); assert(nattempts<64); attempts[nattempts++]=time_ms;
    snprintf(last_uri,sizeof last_uri,"%s",c->uri); last_attach=c->crt_bundle_attach;
    if(fail_init)return NULL;
    allocations++; return calloc(1,sizeof(struct MockWS));
}
static int esp_websocket_register_events(esp_websocket_client_handle_t ws,int event,void(*cb)(void*,esp_event_base_t,int32_t,void*),void *arg) { (void)event; ws->cb=cb; ws->arg=arg; return fail_register?-1:0; }
static int esp_websocket_client_start(esp_websocket_client_handle_t ws) { if(stop_during_ws_start) { stop_during_ws_start=false; atomic_store(&s_wifi_allowed,false); } starts++; if(fail_start)return -1; if(connect_on_start)ws->cb(ws->arg,NULL,WEBSOCKET_EVENT_CONNECTED,NULL); return 0; }
static int esp_websocket_client_stop(esp_websocket_client_handle_t ws) { if(ws->cb)ws->cb(ws->arg,NULL,WEBSOCKET_EVENT_DISCONNECTED,NULL); return 0; }
static int esp_websocket_client_destroy(esp_websocket_client_handle_t ws) { destroys++; free(ws); return 0; }
static int esp_websocket_client_send_text(esp_websocket_client_handle_t ws,const char *text,int len,unsigned wait) {
    (void)wait; sends++; assert(len<=LINK_JSON_MAX); memcpy(ws_last,text,len); ws_last[len]=0;
    cJSON *j=cJSON_ParseWithLength(text,len); const char *t=cJSON_GetStringValue(cJSON_GetObjectItem(j,"t"));
    if(auto_welcome&&t&&!strcmp(t,"hello")) {
        const char *welcome="{\"t\":\"welcome\",\"session\":\"wifi\"}";
        esp_websocket_event_data_t d={.op_code=1,.payload_len=(int)strlen(welcome),.data_len=(int)strlen(welcome),.data_ptr=welcome};
        ws->cb(ws->arg,NULL,WEBSOCKET_EVENT_DATA,&d);
    }
    cJSON_Delete(j); return len;
}
static int esp_websocket_client_send_bin(esp_websocket_client_handle_t ws,const void *p,int len,unsigned wait) { (void)ws;(void)wait; assert(len>=6&&len<=485&&((const uint8_t *)p)[0]==4); sends++; return len; }

// Native Muse is independent of the host protocol; USB config remains usable.
static bool muse_selected;
static uint32_t muse_sent_session;
static void muse_backend_init(const link_handlers_t *h, void (*route)(bool)) { (void)h; (void)route; }
static bool muse_backend_selected(void) { return muse_selected; }
static bool muse_backend_wifi_stopped(void) { return true; }
static void muse_backend_allow(bool value) { (void)value; }
static bool muse_backend_json(const char *text, uint32_t session) { (void)text; muse_sent_session=session; return true; }
static bool muse_backend_pcm(uint8_t turn, const int16_t *pcm, size_t bytes, const uint8_t *ima, uint32_t session) {
    (void)turn; (void)pcm; (void)bytes; (void)ima; muse_sent_session=session; return true;
}
