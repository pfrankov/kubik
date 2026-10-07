// Native Muse adapter acceptance: real JSON/NVS validation, pairing flow and control framing.
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "muse_store.h"
#include "muse_json.h"
#include "muse_vm.h"
#include "settings.h"
#include "version.h"
#include "setup_request.h"

typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
static unsigned char saved_blob[5000], staged_blob[5000];
static size_t blob_size, staged_size;
static bool nvs_fail;
static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle) { assert(!strcmp(ns,"kubik")); (void)mode; *handle=1; return ESP_OK; }
static void nvs_close(nvs_handle_t handle) { assert(handle==1); }
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *size) {
    assert(h==1&&!strcmp(key,"muse_v1"));
    if(!blob_size)return ESP_ERR_NVS_NOT_FOUND;
    if(*size<blob_size)return ESP_ERR_INVALID_ARG;
    memcpy(data,saved_blob,blob_size);*size=blob_size;return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size) {
    assert(h==1&&!strcmp(key,"muse_v1")&&size<=sizeof staged_blob);
    if(nvs_fail)return ESP_FAIL;
    memcpy(staged_blob,data,size);staged_size=size;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) { assert(h==1);if(nvs_fail)return ESP_FAIL;memcpy(saved_blob,staged_blob,staged_size);blob_size=staged_size;return ESP_OK; }
static unsigned wipes;
static void mbedtls_platform_zeroize(void *data,size_t size) { volatile unsigned char *p=data;while(size--)*p++=0;wipes++; }

settings_t g_settings;
static int64_t fake_us;
static int64_t esp_timer_get_time(void) { return fake_us; }
static uint32_t esp_random(void) { return 123; }
void muse_device_identity(char *node,unsigned nc,char *device,unsigned dc) { snprintf(node,nc,"homelink-test");snprintf(device,dc,"hatch-link:test"); }
static int64_t request_id;
static uint8_t sent_frame[4096];static size_t sent_size;
static void (*response_callback)(void *,int,const uint8_t *,size_t,bool);
static int64_t muse_link_req_open(const char *verb,const char *path,const char *const *headers,bool end,
    void (*callback)(void *,int,const uint8_t *,size_t,bool),void *ctx) {
    assert(!strcmp(verb,"POST")&&!strcmp(path,"/link-control")&&!headers&&!end&&!ctx);
    response_callback=callback;return ++request_id;
}
static bool muse_link_req_send(int64_t id,const void *bytes,size_t size,bool end,int wait) {
    assert(id==request_id&&size<sizeof sent_frame&&!end&&wait==200);
    memcpy(sent_frame,bytes,size);sent_size=size;return true;
}
static void muse_link_req_cancel(int64_t id) { assert(id==request_id); }

static int settings_result=ESP_OK;
int settings_wifi_find(const char *ssid) {
    for(unsigned i=0;i<g_settings.wifi_profile_count;i++)if(!strcmp(ssid,g_settings.wifi_profiles[i].ssid))return i;
    return -1;
}
esp_err_t settings_save_connection(const char *ssid,const char *password,const char *url) {
    if(settings_result!=ESP_OK)return settings_result;
    snprintf(g_settings.wifi_profiles[0].ssid,33,"%s",ssid);
    snprintf(g_settings.wifi_profiles[0].password,65,"%s",password);
    g_settings.wifi_profile_count=1;
    snprintf(g_settings.server_url,sizeof g_settings.server_url,"%s",url);return ESP_OK;
}
static char attempted_ssid[33];static bool join_ok, expire_join;
static unsigned sleeps;
static void wifi_try(const char *ssid,const char *password) { (void)password;snprintf(attempted_ssid,sizeof attempted_ssid,"%s",ssid?ssid:""); }
static bool wifi_sta_connected(void) { return join_ok&&sleeps>=1; }
static int wifi_attempts(void) { return 0; }
static int wifi_scan(void *out,int cap) { (void)out;(void)cap;return 0; }
typedef struct {char ssid[33];int8_t rssi;bool secure;} wifi_net_t;
typedef int SemaphoreHandle_t;
#define portMAX_DELAY 1
#define pdMS_TO_TICKS(x) (x)
static int xSemaphoreTake(int h,int wait) { (void)h;(void)wait;return 1; }
static int xSemaphoreGive(int h) { (void)h;return 1; }
static int xSemaphoreCreateMutex(void) { return 1; }
static void vTaskDelay(unsigned ms) { fake_us+=(int64_t)ms*1000;sleeps++; }
static unsigned restarts;
static void esp_restart(void) { restarts++; }
#define ESP_MAC_WIFI_STA 1
#define MAC2STR(m) (m)[0],(m)[1],(m)[2],(m)[3],(m)[4],(m)[5]
static void esp_read_mac(uint8_t *mac,int kind) { (void)kind;memset(mac,1,6); }
static struct {int unused;} g_face;
static void face_lock(void){}
static void face_unlock(void){}
static void face_card(void *face,const char *text){(void)face;(void)text;}
static void disp_wake(void){}
static unsigned generation=1;
static bool confirmation_armed, confirmed;
static const char *link_pairing_handle_client_hello(cJSON *o,char **reply){(void)o;*reply=NULL;return "unsupported";}
static const char *link_pairing_decrypt_command(cJSON *o,char **p){(void)o;*p=NULL;return "unsupported";}
static bool link_pairing_session_confirmed(void){return confirmed;}
static uint32_t link_pairing_session_generation(void){return generation;}
static bool link_pairing_provisioning_session_valid(uint32_t g){return g==generation&&!expire_join;}
static uint32_t link_pairing_mark_provisioning_active(void){return generation;}
static bool link_pairing_commit_provisioning(uint32_t g,bool (*commit)(void)){return g==generation&&!expire_join&&commit();}
static uint32_t link_pairing_handle_client_finished(void){return generation;}
static bool link_pairing_arm_confirmation(uint32_t g){confirmation_armed=g==generation;return confirmation_armed;}
static uint32_t link_pairing_confirm_active_session(void){if(!confirmation_armed)return 0;confirmed=true;return generation;}
static char statuses[8][48];static unsigned status_count;
static char *link_pairing_encrypt_status(const char *status,uint32_t g,uint32_t *record){
    assert(g==generation&&status_count<8);snprintf(statuses[status_count++],48,"%s",status);*record=g;return strdup("encrypted");
}
static char *link_pairing_encrypt_json(const char *plain,uint32_t g,uint32_t *record){*record=g;return strdup(plain);}
static void link_pairing_add_device_info(cJSON *o){(void)o;}
static void link_pairing_init(const char *n,const char *d,const char *m,const char *v,const char *t){(void)n;(void)d;(void)m;(void)v;(void)t;}
static bool muse_ble_current(uint32_t epoch){return epoch==1;}
static bool muse_ble_send(const char *json,uint32_t record){(void)json;(void)record;return true;}
static void muse_ble_disconnect(void){}
static esp_err_t muse_ble_start(const char *name,void (*dispatch)(const char *,bool,uint32_t)){(void)name;(void)dispatch;return ESP_OK;}
static int httpd_req_recv(httpd_req_t *r,char *body,size_t size){
    if (r->fail) return -1;
    if (size > 7) {
        size = 7;
    }
    memcpy(body, r->body + r->offset, size);
    r->offset += size;
    return (int)size;
}

/* PRODUCTION */

static char token[49];
static cJSON *parse(const char *text){cJSON *o=muse_json_parse(text,strlen(text));assert(o);return o;}
static const char *str(const cJSON *o,const char *key){return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,key));}
static void check_store(void) {
    strcpy(token,"mgst_");memset(token+5,'A',43);token[48]=0;
    assert(muse_sdk_token_valid(token));token[47]='B';assert(!muse_sdk_token_valid(token));token[47]='A';
    assert(!muse_sdk_token_valid("mgst_short")&&!muse_sdk_token_valid(NULL));
    assert(muse_store_begin(token)==ESP_OK&&muse_store_state()==MUSE_PAIRING);
    muse_credentials_t loaded;assert(muse_store_load(&loaded)==ESP_OK);
    loaded.state=MUSE_PAIRED;strcpy(loaded.access_token,"access");strcpy(loaded.refresh_token,"refresh");
    assert(muse_account_token_valid("hatch_refresh:base64-url_token"));
    assert(!muse_account_token_valid(NULL)&&!muse_account_token_valid("access\r\nInjected: header"));
    strcpy(loaded.access_token,"bad\nheader");assert(muse_store_save(&loaded)==ESP_ERR_INVALID_ARG);
    strcpy(loaded.access_token,"access");
    assert(muse_store_save(&loaded)==ESP_OK&&muse_store_state()==MUSE_PAIRED);
    assert(muse_store_select(false)==ESP_OK&&muse_store_state()==MUSE_OFF&&muse_store_saved_state()==MUSE_PAIRED);
    assert(muse_store_select(true)==ESP_OK&&muse_store_load(&loaded)==ESP_OK&&!strcmp(loaded.refresh_token,"refresh"));
    nvs_fail=true;assert(muse_store_begin(token)==ESP_FAIL);nvs_fail=false;
    assert(muse_store_state()==MUSE_PAIRED);
    saved_blob[0]^=1;memset(&loaded,0x55,sizeof loaded);assert(muse_store_load(&loaded)==ESP_ERR_INVALID_STATE);
    for(size_t i=0;i<sizeof loaded;i++)assert(!((unsigned char *)&loaded)[i]);
    saved_blob[0]^=1;
}
static void check_json(void) {
    assert(!muse_json_parse("{}{}",4));assert(!muse_json_parse("{\"key\":\"a\\u0000b\"}",20));
    char deep[40];memset(deep,'[',17);memset(deep+17,']',17);assert(!muse_json_parse(deep,34));
    char embedded[]={ '{','}',0,' ' };assert(!muse_json_parse(embedded,4));
    cJSON *o=parse("{\"key\":\"a\\\\u0000b\"}  ");assert(!strcmp(str(o,"key"),"a\\u0000b"));muse_json_clear(o);
    o=parse("{\"text\":\"Tess \\u263a\"}");muse_json_clear(o);
}
static void check_setup(void) {
    setup_agent_choice_t choice;
    cJSON *o=parse("{\"agent\":\"Muse\"}");assert(!setup_agent_choice(o,&choice)&&choice.muse&&!choice.sdk_token[0]);muse_json_clear(o);
    o=parse("{\"agent\":\"Muse\",\"sdk_token\":\"invalid\"}");assert(!strcmp(setup_agent_choice(o,&choice),"sdk_token"));muse_json_clear(o);
    o=cJSON_CreateObject();cJSON_AddStringToObject(o,"agent","Muse");cJSON_AddStringToObject(o,"sdk_token",token);
    assert(!setup_agent_choice(o,&choice)&&!strcmp(choice.sdk_token,token));muse_json_clear(o);
    assert(setup_agent_save(&choice)==ESP_OK&&muse_store_state()==MUSE_PAIRING);
    o=parse("{\"agent\":\"Hermes\"}");assert(!strcmp(setup_agent_choice(o,&choice),"url"));muse_json_clear(o);
    o=parse("{\"agent\":\"Hermes\",\"url\":\"kubik://host:18793\"}");assert(!setup_agent_choice(o,&choice)&&!choice.muse);
    assert(setup_agent_save(&choice)==ESP_OK&&muse_store_state()==MUSE_OFF&&muse_store_saved_state()==MUSE_PAIRING);muse_json_clear(o);
    assert(!setup_validate_credentials("Home","validpass"));assert(!strcmp(setup_validate_credentials("Home","short"),"password_format"));
    const char *body="{\"agent\":\"Muse\"}";httpd_req_t req={.content_len=(int)strlen(body),.body=body};
    unsigned before=wipes;o=setup_read_request(&req);assert(o&&wipes>before);muse_json_clear(o);
    req.offset=0;req.fail=true;assert(!setup_read_request(&req));
}
static void check_options(void) {
    cJSON *o=parse("{\"t\":\"agent_options\",\"target\":\"mode\",\"rid\":42}");cJSON *reply=muse_options_reply(o,true);
    assert(reply&&!strcmp(str(reply,"model"),"classic"));
    cJSON *models=cJSON_GetObjectItem(reply,"models");assert(cJSON_GetArraySize(models)==3);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(models,0),"available")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(models,1),"available")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(reply,"tts"),"available")));
    muse_json_clear(o);muse_json_clear(reply);
    o=parse("{\"t\":\"agent_model\",\"target\":\"mode\",\"rid\":43,\"id\":\"live\"}");reply=muse_options_reply(o,true);
    assert(!strcmp(str(reply,"error"),"unsupported"));muse_json_clear(reply);muse_json_clear(o);
}
static char notified[1024];
static void receive_notification(const char *text){snprintf(notified,sizeof notified,"%s",text);}
static cJSON *last_sent(void){uint32_t n=length(sent_frame);assert(n+4==sent_size);return muse_json_parse((char *)sent_frame+4,n);}
static void feed(const char *text){size_t n=strlen(text);uint8_t bytes[4096];for(unsigned i=0;i<4;i++)bytes[i]=n>>(i*8);memcpy(bytes+4,text,n);
    for(size_t i=0;i<n+4;i++)response_callback(NULL,0,bytes+i,1,false);
}
static void check_registration_errors(void) {
    const char *errors[]={"null","false","true","\"denied\""};
    for(unsigned i=0;i<4;i++) {
        assert(muse_control_start(receive_notification));char ack[180];
        snprintf(ack,sizeof ack,"{\"type\":\"res\",\"id\":\"%s\",\"error\":%s,\"result\":{\"status\":\"registered\"}}",registration,errors[i]);
        feed(ack);assert(muse_control_ready()==(i<2));assert(muse_control_failed()==(i>=2));muse_control_clear();
    }
}
static void check_control(void) {
    assert(muse_control_start(receive_notification));cJSON *o=last_sent();assert(o&&!strcmp(str(o,"method"),"link.register"));
    assert(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o,"params"),"is_wakeup_supported")));muse_json_clear(o);
    char ack[160];snprintf(ack,sizeof ack,"{\"type\":\"res\",\"id\":\"%s\",\"result\":{\"status\":\"registered\"}}",registration);
    feed(ack);assert(muse_control_ready());
    feed("{\"type\":\"req\",\"id\":\"notify-1\",\"method\":\"link.invoke\",\"command\":\"device.notify\",\"params\":{\"text\":\"Hello\"}}");
    assert(!strcmp(notified,"Hello"));o=last_sent();assert(o&&!strcmp(str(o,"method"),"link.result")&&!strcmp(str(o,"id"),"notify-1"));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(o,"ok"))&&!cJSON_GetObjectItem(o,"type")&&!cJSON_GetObjectItem(o,"params"));muse_json_clear(o);
    feed("{\"id\":\"bad\",\"method\":\"link.invoke\",\"command\":\"device.exec\",\"params\":{}}");o=last_sent();assert(cJSON_IsFalse(cJSON_GetObjectItem(o,"ok"))&&str(o,"error"));muse_json_clear(o);
    const char *many="{\"method\":\"link.invoke\",\"id\":\"batch\",\"command\":\"device.notify\",\"params\":{\"text\":\"Hi\"}}";
    uint8_t batch[8192];size_t one=strlen(many)+4;
    for(unsigned i=0;i<60;i++) {
        for(unsigned b=0;b<4;b++)batch[i*one+b]=(one-4)>>(b*8);
        memcpy(batch+i*one+4,many,one-4);
    }
    assert(one*60>CONTROL_MAX);response_callback(NULL,0,batch,one*60,false);assert(muse_control_ready());
    uint8_t bad_length[4]={0xff,0xff,0xff,0xff};response_callback(NULL,0,bad_length,4,false);assert(muse_control_failed());muse_control_clear();
    assert(muse_control_start(receive_notification));fake_us+=21000000;assert(muse_control_failed());muse_control_clear();
    check_registration_errors();
}
static cJSON *provision_message(void){return parse("{\"ssid\":\"Home\",\"password\":\"validpass\",\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"token_type\":\"device\"}");}
static void check_provision(void) {
    cJSON *o;
    status_count=0;join_ok=false;sleeps=0;o=provision_message();provision(o);muse_json_clear(o);
    assert(!restarts&&!strcmp(statuses[0],"wifi_connecting")&&!strcmp(statuses[1],"wifi_failed"));
    assert(muse_store_saved_state()==MUSE_PAIRING&&!attempted_ssid[0]);
    status_count=0;join_ok=true;expire_join=true;sleeps=0;o=provision_message();provision(o);muse_json_clear(o);
    assert(!restarts&&muse_store_saved_state()==MUSE_PAIRING);expire_join=false;
    status_count=0;sleeps=0;o=provision_message();provision(o);muse_json_clear(o);
    assert(restarts==1&&status_count==3&&!strcmp(statuses[0],"wifi_connecting")&&!strcmp(statuses[1],"wifi_connected")&&!strcmp(statuses[2],"auth_ok"));
    assert(muse_store_saved_state()==MUSE_PAIRED&&!strcmp(g_settings.wifi_profiles[0].ssid,"Home"));
    status_count=0;o=provision_message();cJSON_AddStringToObject(o,"noise_host","evil.example");provision(o);muse_json_clear(o);
    assert(restarts==1&&!strcmp(statuses[0],"error_invalid_command"));
    unsigned before=wipes;dispatch_command("{\"access_token\":\"private\"}",true,1);assert(wipes>before);
}
static void check_pair(void) {
    assert(muse_store_select(true)==ESP_OK);
    credentials=calloc(1,sizeof *credentials);assert(credentials&&muse_store_load(credentials)==ESP_OK);active=true;
    cJSON *o=parse("{\"action\":\"pairing_client_finished\"}");finish_handshake(o);muse_json_clear(o);
    assert(status_count==1&&!strcmp(statuses[0],"confirm_required")&&confirmation_armed);
    assert(muse_pair_key()&&confirmed&&!strcmp(statuses[1],"pairing_confirmed"));
    check_provision();
    muse_store_wipe(credentials,sizeof *credentials);free(credentials);credentials=NULL;
}
int main(void) {
    strcpy(g_settings.name,"Kubik");check_store();check_json();check_setup();check_options();check_control();check_pair();
    puts("muse: credential isolation, strict JSON, phone setup, capabilities, fragmented control and pairing Wi-Fi/status flow passed");
}
