#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_TIMEOUT -4
#define ESP_ERR_NVS_NOT_FOUND -5
#define ESP_LOGE(...) ((void)0)
static void fake_log(const char *tag,const char *format,...) { (void)tag;(void)format; }
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#ifndef MOCK_HANDLE
#define MOCK_HANDLE 42
#endif
#define BLE_ADDR_RANDOM 1
#define BLE_HS_ETIMEOUT 4
#define BLE_HS_EDONE 14
#define BLE_HS_ENOTCONN 7
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_ERR_REM_USER_CONN_TERM 19
#define BLE_ERR_CONN_PARMS 59
#define BLE_GATT_CHR_PROP_WRITE 8
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP 4
#define BLE_GATT_CHR_PROP_NOTIFY 16
#define BLE_GATT_DSC_CLT_CFG_UUID16 0x2902
#define BLE_GAP_EVENT_DISC 7
#define BLE_GAP_EVENT_DISC_COMPLETE 8
#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_L2CAP_UPDATE_REQ 5
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_NOTIFY_RX 12
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef uint32_t TickType_t;
typedef struct { bool available; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef struct { uint8_t type, val[6]; } ble_addr_t;
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; } ble_uuid_any_t;
#define BLE_UUID16_DECLARE(v) (&(ble_uuid16_t){.value=(v)}.u)
struct ble_hs_adv_fields { const uint8_t *name; uint8_t name_len; const ble_uuid16_t *uuids16; uint8_t num_uuids16; };
struct os_mbuf { const uint8_t *bytes; size_t size; };
#define OS_MBUF_PKTLEN(m) ((m)->size)
struct ble_gatt_error { int status; };
struct ble_gatt_svc { uint16_t start_handle, end_handle; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_attr { uint16_t handle; };
struct ble_gap_disc_desc { ble_addr_t addr; const uint8_t *data; uint8_t length_data; };
struct ble_gap_disc_params { uint16_t itvl, window; uint8_t filter_duplicates, passive; };
struct ble_gap_conn_params {
    uint16_t scan_itvl, scan_window, itvl_min, itvl_max, latency, supervision_timeout, min_ce_len, max_ce_len;
};
struct ble_gap_event {
    int type;
    struct ble_gap_disc_desc disc;
    struct { int reason; } disc_complete;
    struct { int status; uint16_t conn_handle; } connect;
    struct { uint16_t conn_handle, attr_handle; struct os_mbuf *om; } notify_rx;
};
typedef int gap_fn(struct ble_gap_event *, void *);
typedef int mtu_fn(uint16_t,const struct ble_gatt_error *,uint16_t,void *);
typedef int svc_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_svc *,void *);
typedef int chr_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_chr *,void *);
typedef int dsc_fn(uint16_t,const struct ble_gatt_error *,uint16_t,const struct ble_gatt_dsc *,void *);
typedef int attr_fn(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *);
typedef int nvs_handle_t;
static int nvs_open(const char *,int,nvs_handle_t *);
static int nvs_get_blob(int,const char *,void *,size_t *);
static int nvs_set_blob(int,const char *,const void *,size_t);
static int nvs_commit(int);
static int nvs_erase_key(int,const char *);
static void nvs_close(int);
static int app_nvs_lock(void);
static void app_nvs_unlock(void);
static bool app_nvs_ready_locked(void);
static int app_nvs_recover_locked(void);
static void app_nvs_mark_unhealthy_locked(void);
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) { s->available=false; return s; }
static int64_t fake_time;
static void (*wait_hook)(void);
static int64_t esp_timer_get_time(void) { return fake_time; }
static int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    if (!s->available && ticks && wait_hook) { void (*run)(void)=wait_hook; wait_hook=NULL; run(); }
    if (s->available) { s->available=false; return pdTRUE; }
    fake_time += (int64_t)ticks*1000; return 0;
}
static void xSemaphoreGive(SemaphoreHandle_t s) { s->available=true; }
static int ble_uuid_u16(const ble_uuid_t *u) { return ((const ble_uuid16_t *)u)->value; }
static int ble_hs_mbuf_to_flat(const struct os_mbuf *m,void *p,uint16_t n,uint16_t *size) {
    if(m->size>n) { return -1; }
    memcpy(p,m->bytes,m->size);*size=m->size;return 0;
}
static int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f,const uint8_t *p,uint8_t n) {
    assert(n==sizeof(*f));memcpy(f,p,n);return 0;
}
static void ble_svc_gap_init(void) {}
static void ble_svc_gatt_init(void) {}
static int ble_att_set_preferred_mtu(int n) { assert(n==185);return 0; }
static int ble_hs_util_ensure_addr(int n) { assert(!n);return 0; }
static int ble_hs_id_infer_auto(int n,uint8_t *p) { assert(!n);*p=0;return 0; }
static int ble_gap_disc(uint8_t,int,const struct ble_gap_disc_params *,gap_fn *,void *);
static int ble_gap_connect(uint8_t,const ble_addr_t *,int,const struct ble_gap_conn_params *,gap_fn *,void *);
static int ble_gattc_exchange_mtu(uint16_t,mtu_fn *,void *);
static int ble_gattc_disc_svc_by_uuid(uint16_t,const ble_uuid_t *,svc_fn *,void *);
static int ble_gattc_disc_all_chrs(uint16_t,uint16_t,uint16_t,chr_fn *,void *);
static int ble_gattc_disc_all_dscs(uint16_t,uint16_t,uint16_t,dsc_fn *,void *);
static int ble_gattc_write_flat(uint16_t,uint16_t,const void *,uint16_t,attr_fn *,void *);
static int ble_gattc_write_no_rsp_flat(uint16_t,uint16_t,const void *,uint16_t);

#include "actual.inc"

static int fake_owner, starts, stops, sends, writes, commits, scans, connects;
static bool persisted, namespace_present, bad_store, fail_stop, ambiguous, absent, small_mtu, no_write, bad_fw, no_reply, nack, late_reply, oversized_reply, bad_battery;
static bool nvs_locked, nvs_ready=true, user_settings=true, cancel_scan, cancel_write, spurious_write, spurious_query;
static bool silent_connect, controller_link, cancel_connect, no_no_rsp, cancel_pace;
static bool unknown_hardware, unknown_firmware;
static uint8_t battery_value=100;
static int64_t last_enqueue_time;
static uint8_t last_packet[133];
static size_t last_packet_size;
static uint8_t peer_blob[7];
static const ble_addr_t first_peer={0,{1,2,3,4,5,6}}, second_peer={0,{6,5,4,3,2,1}};
static gap_fn *last_gap;
static void *last_epoch;
static attr_fn *last_writer;
static void *last_write_token;
static uint8_t deferred_response[33];
static size_t deferred_size;
static int64_t expected_deadline;
static int startup_delay_ms, discovery_delay_ms, last_connect_ms;
static peripage_result_t open_for_test(peripage_info_t *info, uint32_t timeout_ms) {
    expected_deadline=fake_time+(int64_t)timeout_ms*1000;
    return peripage_open(info,timeout_ms);
}
static int app_nvs_lock(void) { assert(!nvs_locked);nvs_locked=true;return ESP_OK; }
static void app_nvs_unlock(void) { assert(nvs_locked);nvs_locked=false; }
static bool app_nvs_ready_locked(void) { assert(nvs_locked);return nvs_ready; }
static int app_nvs_recover_locked(void) { assert(nvs_locked);return ESP_OK; }
static void app_nvs_mark_unhealthy_locked(void) { assert(nvs_locked);nvs_ready=false; }
bool ble_runtime_owned(ble_runtime_owner_t owner) { return fake_owner==(int)owner; }
esp_err_t ble_runtime_start(ble_runtime_owner_t owner,ble_runtime_prepare_t prepare,void (*sync)(void)) {
    assert(!fake_owner);fake_owner=owner;last_enqueue_time=-1;starts++;assert(prepare()==ESP_OK);
    fake_time+=(int64_t)startup_delay_ms*1000;sync();return ESP_OK;
}
esp_err_t ble_runtime_stop(ble_runtime_owner_t owner) {
    assert(fake_owner==(int)owner);stops++;if(fail_stop)return ESP_ERR_TIMEOUT;
    controller_link=false;fake_owner=0;return ESP_OK;
}
static int nvs_open(const char *name,int mode,int *handle) {
    assert(nvs_locked&&!strcmp(name,"kubik"));if(mode==NVS_READONLY&&!namespace_present)return ESP_ERR_NVS_NOT_FOUND;
    *handle=1;return 0;
}
static int nvs_get_blob(int h,const char *key,void *p,size_t *n) {
    assert(nvs_locked&&h==1&&!strcmp(key,"printer_peer"));if(!persisted)return ESP_ERR_NVS_NOT_FOUND;
    assert(*n==7);memcpy(p,peer_blob,7);*n=7;return 0;
}
static int nvs_set_blob(int h,const char *key,const void *p,size_t n) {
    assert(nvs_locked&&!fake_owner&&h==1&&!strcmp(key,"printer_peer")&&n==7);if(bad_store)return -1;
    memcpy(peer_blob,p,7);persisted=namespace_present=true;return 0;
}
static int nvs_commit(int h) { assert(nvs_locked&&h==1);commits++;return 0; }
static int nvs_erase_key(int h,const char *key) { assert(nvs_locked&&h==1&&fake_owner!=BLE_RUNTIME_PRINTER&&!strcmp(key,"printer_peer"));persisted=false;return 0; }
static void nvs_close(int h) { assert(nvs_locked&&h==1); }
static void advertisement(gap_fn *cb,void *arg,const ble_addr_t *address,bool service,bool name) {
    const ble_uuid16_t uuid={.value=0xfee7};
    struct ble_hs_adv_fields fields={.name=(const uint8_t *)"PeriPage+2E34_BLE",.name_len=name?17:0,.uuids16=&uuid,.num_uuids16=service?1:0};
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,.disc={.addr=*address,.data=(const uint8_t *)&fields,.length_data=sizeof(fields)}};
    cb(&event,arg);
}
static int ble_gap_disc(uint8_t type,int ms,const struct ble_gap_disc_params *p,gap_fn *cb,void *arg) {
    assert(type==0&&ms==3000&&!p->passive&&p->filter_duplicates==1&&p->itvl==48&&p->window==24);
    fake_time+=(int64_t)discovery_delay_ms*1000;
    scans++;last_gap=cb;last_epoch=arg;
    if(cancel_scan)peripage_cancel();
    if(!absent){advertisement(cb,arg,&first_peer,true,false);advertisement(cb,arg,&first_peer,false,true);}
    if(ambiguous){advertisement(cb,arg,&second_peer,true,true);}
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC_COMPLETE};cb(&event,arg);return 0;
}
static int ble_gap_connect(uint8_t type,const ble_addr_t *address,int ms,const struct ble_gap_conn_params *params,gap_fn *cb,void *arg) {
    assert(!type&&params&&ms>0&&!memcmp(address,&first_peer,sizeof(*address)));
    assert(params->scan_itvl==32&&params->scan_window==16);
    assert(params->itvl_min==24&&params->itvl_max==40&&params->latency==0);
    assert(params->supervision_timeout==256&&params->min_ce_len==0&&params->max_ce_len==0);
    assert(ms==(expected_deadline-fake_time)/1000);last_connect_ms=ms;
    connects++;last_gap=cb;last_epoch=arg;
    if(cancel_connect){peripage_cancel();return 0;}
    if(absent)return BLE_HS_ETIMEOUT;
    controller_link=true;if(silent_connect)return 0;
    struct ble_gap_event update={.type=BLE_GAP_EVENT_L2CAP_UPDATE_REQ};
    assert(cb(&update,arg)==BLE_ERR_CONN_PARMS);
    struct ble_gap_event event={.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=MOCK_HANDLE}};cb(&event,arg);
    assert(cb(&update,arg)==0);return 0;
}
static int ble_gattc_exchange_mtu(uint16_t c,mtu_fn *cb,void *arg) {
    assert(c==MOCK_HANDLE);struct ble_gatt_error e={0};return cb(c,&e,small_mtu?23:136,arg);
}
static int ble_gattc_disc_svc_by_uuid(uint16_t c,const ble_uuid_t *u,svc_fn *cb,void *arg) {
    assert(c==MOCK_HANDLE&&ble_uuid_u16(u)==0xff00);struct ble_gatt_error e={0};struct ble_gatt_svc s={12,20};cb(c,&e,&s,arg);e.status=BLE_HS_EDONE;return cb(c,&e,NULL,arg);
}
static int ble_gattc_disc_all_chrs(uint16_t c,uint16_t start,uint16_t end,chr_fn *cb,void *arg) {
    assert(c==MOCK_HANDLE&&start==12&&end==20);struct ble_gatt_error e={0};
    struct ble_gatt_chr ch={13,14,16,{.u16={.value=0xff01}}};cb(c,&e,&ch,arg);
    ch=(struct ble_gatt_chr){16,17,no_write?0:(no_no_rsp?8:12),{.u16={.value=0xff02}}};cb(c,&e,&ch,arg);
    ch=(struct ble_gatt_chr){18,19,16,{.u16={.value=0xff03}}};cb(c,&e,&ch,arg);
    e.status=BLE_HS_EDONE;return cb(c,&e,NULL,arg);
}
static int ble_gattc_disc_all_dscs(uint16_t c,uint16_t start,uint16_t end,dsc_fn *cb,void *arg) {
    assert(c==MOCK_HANDLE&&start==14&&end==15);struct ble_gatt_error e={0};struct ble_gatt_dsc d={15,{.u16={.value=0x2902}}};cb(c,&e,14,&d,arg);e.status=BLE_HS_EDONE;return cb(c,&e,14,NULL,arg);
}
static void notify(const uint8_t *bytes,size_t size) {
    struct os_mbuf m={bytes,size};struct ble_gap_event e={.type=BLE_GAP_EVENT_NOTIFY_RX,.notify_rx={MOCK_HANDLE,14,&m}};
    last_gap(&e,last_epoch);
}
static void deferred_notify(void) { notify(deferred_response,deferred_size); }
static void deferred_cancel(void) { peripage_cancel(); }
static void spurious_pacer_wake(void) { xSemaphoreGive(event_ready); }
static void deliver_query(const uint8_t *bytes,size_t size) {
    if(!spurious_query){notify(bytes,size);return;}
    memcpy(deferred_response,bytes,size);deferred_size=size;
    peripage_gatt_lost();wait_hook=deferred_notify;
}
static const char *query_value(const uint8_t *p) {
    if(p[2]==0x20)return bad_fw?"V1.12_203dpi":(unknown_firmware?"V1.13_304dpi":"V1.12_304dpi");
    if(p[2]==0x30)return p[3]==0x10?(unknown_hardware?"v3.38.22_AY":"v3.38.21_AY"):"PeriPage+2E34";
    return NULL;
}
static bool is_query(const uint8_t *p,uint16_t size) { return size==4&&p[0]==0x10&&p[1]==0xff; }
static int reply_query(uint16_t c,attr_fn *cb,void *arg,const uint8_t *p) {
    assert(last_enqueue_time<0||(fake_time-last_enqueue_time>=20000&&p[2]==0x50));
    struct ble_gatt_error e={0};cb(c,&e,NULL,arg);
    if(no_reply)return 0;
    if(oversized_reply){uint8_t excessive[33]={0};notify(excessive,sizeof(excessive));return 0;}
    const char *value=query_value(p);
    if(value)deliver_query((const uint8_t *)value,strlen(value));
    else{const uint8_t battery[]={0,bad_battery?101:battery_value};assert(p[2]==0x50&&p[3]==0xf1);deliver_query(battery,2);}
    return 0;
}
static int ble_gattc_write_flat(uint16_t c,uint16_t h,const void *data,uint16_t n,attr_fn *cb,void *arg) {
    assert(c==MOCK_HANDLE&&(h==15||h==17));writes++;last_writer=cb;last_write_token=arg;
    struct ble_gatt_error e={.status=nack?9:0};if(nack){cb(c,&e,NULL,arg);return 0;}
    const uint8_t *p=data;
    if(h==15){assert(n==2&&p[0]==1&&!p[1]);return cb(c,&e,NULL,arg);}
    assert(is_query(p,n));return reply_query(c,cb,arg,p);
}
static int ble_gattc_write_no_rsp_flat(uint16_t c,uint16_t h,const void *data,uint16_t n) {
    assert(c==MOCK_HANDLE&&h==17&&n<=sizeof last_packet);writes++;sends++;
    assert(last_enqueue_time<0||fake_time-last_enqueue_time>=20000);
    last_enqueue_time=fake_time;last_packet_size=n;memcpy(last_packet,data,n);
    if(late_reply||nack)return BLE_HS_ETIMEOUT;
    if(cancel_write){peripage_cancel();return 0;}
    if(cancel_pace)wait_hook=deferred_cancel;
    if(spurious_write)wait_hook=spurious_pacer_wake;
    return 0;
}

static void assert_closed(void) { assert(!fake_owner&&!radio_started&&!controller_link&&connection==BLE_HS_CONN_HANDLE_NONE); }

static void cancellation_checks(peripage_info_t *info,uint8_t row[72]) {
    cancel_connect=true;assert(open_for_test(info,12000)==PERIPAGE_CANCELLED);assert_closed();cancel_connect=false;
    assert(peripage_forget_binding());
    cancel_scan=true;assert(open_for_test(info,12000)==PERIPAGE_CANCELLED);assert_closed();cancel_scan=false;
    assert(open_for_test(info,12000)==PERIPAGE_OK);cancel_write=true;
    assert(!peripage_write_packet(row,72,1000));int previous=writes;
    assert(!peripage_write_packet(row,72,1000)&&writes==previous);
    assert(peripage_close());assert_closed();cancel_write=false;
    peripage_cancel();assert_closed();
    spurious_query=true;assert(open_for_test(info,12000)==PERIPAGE_OK);spurious_query=false;
    spurious_write=true;assert(peripage_write_packet(row,72,1000));spurious_write=false;
    assert(peripage_close());assert_closed();
}

static void profile_checks(const peripage_info_t *info) {
    assert(info->dpi==304);assert(info->width_dots==576);
    assert(info->feed_dpi==152);assert(info->charging==-1);
    assert(info->mtu==136);assert(info->battery_pct==100);
    assert(!strcmp(info->name,"PeriPage+2E34"));
}

static void missing_connect_event(peripage_info_t *info) {
    // IDF can create the controller link before remote-feature/version events
    // finish. Host stop must still own its disconnect when CONNECT never arrives.
    silent_connect=true;assert(open_for_test(info,12000)==PERIPAGE_FAILED);
    assert_closed();silent_connect=false;
    assert(open_for_test(info,12000)==PERIPAGE_OK);
    assert(peripage_close());assert_closed();
}

static void refreshed_battery_checks(peripage_info_t *info) {
    int queries=writes;peripage_info_t expected=*info;
    expected.battery_pct=73;battery_value=73;
    assert(peripage_finish_packet_stream(3000,info)&&writes==queries+1);
    assert(!memcmp(info,&expected,sizeof expected));battery_value=100;
    assert(!peripage_finish_packet_stream(3001,info)&&!peripage_finish_packet_stream(0,info));
    queries=writes;assert(!peripage_finish_packet_stream(3000,NULL)&&writes==queries);
}

static void stream_checks(peripage_info_t *info,uint8_t row[72]) {
    assert(peripage_packet_stream_budget_ms(0)==6000);
    assert(peripage_packet_stream_budget_ms(1867)==43340);
    assert(peripage_packet_stream_budget_ms(SIZE_MAX)==UINT32_MAX);
    assert(open_for_test(info,12000)==PERIPAGE_OK);
    int64_t start=fake_time;int before=sends;
    const uint8_t reset[16]={0x10,0xff,0xfe,1},density[5]={0x10,0xff,0x10,0,1},header[8]={0x1d,0x76,0x30,0,72,0,1,0};
    assert(peripage_write_packet(reset,sizeof reset,1000)&&last_packet_size==sizeof reset&&!memcmp(last_packet,reset,sizeof reset));
    assert(peripage_write_packet(density,sizeof density,1000)&&last_packet_size==sizeof density);
    assert(peripage_write_packet(header,sizeof header,1000)&&last_packet_size==sizeof header);
    assert(peripage_write_packet(row,72,1000)&&last_packet_size==72&&!memcmp(last_packet,row,72));
    assert(sends==before+4&&fake_time-start==80000);
    fake_time+=100000;start=fake_time;
    assert(peripage_write_packet(row,72,1000)&&fake_time-start==20000); // A late caller never catches up with a burst.
    refreshed_battery_checks(info);
    assert(peripage_close());assert_closed();
    assert(open_for_test(info,12000)==PERIPAGE_OK);cancel_pace=true;start=fake_time;
    assert(!peripage_write_packet(row,72,1000)&&fake_time-start<=20000);
    assert(!peripage_finish_packet_stream(3000,info));cancel_pace=false;
    assert(peripage_close());assert_closed();
    assert(open_for_test(info,12000)==PERIPAGE_OK);bad_battery=true;peripage_info_t expected=*info;
    assert(!peripage_finish_packet_stream(3000,info));bad_battery=false;
    assert(!memcmp(info,&expected,sizeof expected));
    before=sends;assert(!peripage_write_packet(row,72,1000)&&sends==before);
    assert(peripage_close());assert_closed();
}

static void unverified_profile_checks(peripage_info_t *info) {
    unknown_hardware=true;assert(open_for_test(info,12000)==PERIPAGE_UNSUPPORTED);
    assert_closed();unknown_hardware=false;
    unknown_firmware=true;assert(open_for_test(info,12000)==PERIPAGE_UNSUPPORTED);
    assert_closed();unknown_firmware=false;
}

static void finish_failure_checks(peripage_info_t *info,uint8_t row[72]) {
    assert(open_for_test(info,12000)==PERIPAGE_OK);no_reply=true;
    assert(!peripage_finish_packet_stream(3000,info));no_reply=false;
    int previous=writes;assert(!peripage_finish_packet_stream(3000,info)&&writes==previous);
    assert(!peripage_write_packet(row,72,1000)&&writes==previous);
    assert(peripage_close());assert_closed();
    assert(open_for_test(info,12000)==PERIPAGE_OK);oversized_reply=true;
    assert(!peripage_finish_packet_stream(3000,info));oversized_reply=false;
    assert(peripage_close());assert_closed();
    assert(open_for_test(info,12000)==PERIPAGE_OK);previous=sends;
    assert(!peripage_write_packet(row,72,19)&&sends==previous);
    assert(peripage_write_packet(row,72,20));
    assert(peripage_finish_packet_stream(3000,info));assert(peripage_close());assert_closed();
}

int main(void) {
    peripage_info_t info;
    startup_delay_ms=250;discovery_delay_ms=3000;
    assert(open_for_test(&info,12000)==PERIPAGE_OK);
    assert(last_connect_ms==8750); // Startup and discovery consume the original deadline, never extend it.
    startup_delay_ms=discovery_delay_ms=0;
    profile_checks(&info);assert(!persisted);
    assert(!peripage_forget_binding()); // No flash writes while the controller owns memory.
    uint8_t row[72]={0x80};assert(peripage_write_packet(row,sizeof(row),1000)&&sends==1);
    assert(!peripage_write_packet(row,134,1000)&&sends==1);
    assert(peripage_close()&&persisted&&commits==1);assert_closed();
    cancellation_checks(&info,row);assert(user_settings&&!nvs_locked);
    missing_connect_event(&info);
    stream_checks(&info,row);
    finish_failure_checks(&info,row);
    unverified_profile_checks(&info);
    int previous_stops=stops;assert(peripage_close()&&stops==previous_stops);
    int prior_scans=scans, prior_connects=connects;
    absent=ambiguous=true;assert(open_for_test(&info,12000)==PERIPAGE_FAILED);assert_closed();absent=false;
    assert(scans==prior_scans&&connects==prior_connects+1); // Never scan/select a neighbour for a saved peer.
    ambiguous=true;assert(open_for_test(&info,12000)==PERIPAGE_OK); // Bound peer wins; a neighbour is never selected.
    assert(scans==prior_scans&&connects==prior_connects+2);
    assert(peripage_close());bad_fw=true;
    assert(open_for_test(&info,12000)==PERIPAGE_UNSUPPORTED);assert_closed();bad_fw=false;
    assert(scans==prior_scans); // Binding skips advertisements, never fresh GATT/profile verification.
    assert(peripage_forget_binding());
    assert(open_for_test(&info,12000)==PERIPAGE_AMBIGUOUS);assert_closed();ambiguous=false;
    absent=true;assert(open_for_test(&info,12000)==PERIPAGE_NOT_FOUND);assert_closed();absent=false;
    small_mtu=true;assert(open_for_test(&info,12000)==PERIPAGE_UNSUPPORTED);assert_closed();small_mtu=false;
    no_write=true;assert(open_for_test(&info,12000)==PERIPAGE_UNSUPPORTED);assert_closed();no_write=false;
    no_no_rsp=true;assert(open_for_test(&info,12000)==PERIPAGE_UNSUPPORTED);assert_closed();no_no_rsp=false;
    bad_fw=true;assert(open_for_test(&info,12000)==PERIPAGE_UNSUPPORTED);assert_closed();bad_fw=false;
    no_reply=true;assert(open_for_test(&info,12000)==PERIPAGE_FAILED);assert_closed();no_reply=false;
    oversized_reply=true;assert(open_for_test(&info,12000)==PERIPAGE_FAILED);assert_closed();oversized_reply=false;
    bad_battery=true;assert(open_for_test(&info,12000)==PERIPAGE_FAILED);assert_closed();bad_battery=false;
    assert(open_for_test(&info,12000)==PERIPAGE_OK);late_reply=true;
    assert(!peripage_write_packet(row,72,1000));int previous_writes=writes;
    assert(!peripage_write_packet(row,72,1000)&&writes==previous_writes);
    attr_fn *old_callback=last_writer;void *old_token=last_write_token;gap_fn *old_gap=last_gap;void *old_epoch=last_epoch;
    assert(peripage_close());assert_closed();late_reply=false;
    assert(open_for_test(&info,12000)==PERIPAGE_OK);
    struct ble_gatt_error good={0};old_callback(MOCK_HANDLE,&good,NULL,old_token);
    struct ble_gap_event disconnected={.type=BLE_GAP_EVENT_DISCONNECT};old_gap(&disconnected,old_epoch);
    assert(peripage_write_packet(row,72,1000));assert(peripage_close());assert_closed();
    assert(peripage_forget_binding());bad_store=true;
    assert(open_for_test(&info,12000)==PERIPAGE_OK);assert(!peripage_close());assert_closed();bad_store=false;
    nvs_ready=false;int before_nvs_failure=starts;
    assert(open_for_test(&info,12000)==PERIPAGE_FAILED);assert(starts==before_nvs_failure);
    assert_closed();assert(!nvs_locked);nvs_ready=true;
    fake_owner=BLE_RUNTIME_MUSE;int previous_starts=starts;
    assert(open_for_test(&info,12000)==PERIPAGE_BUSY&&starts==previous_starts);
    assert(peripage_forget_binding());fake_owner=0;
    assert(open_for_test(&info,12000)==PERIPAGE_OK);fail_stop=true;
    assert(!peripage_close()&&radio_started&&fake_owner==BLE_RUNTIME_PRINTER);
    assert(open_for_test(&info,12000)==PERIPAGE_BUSY);fail_stop=false;assert(peripage_close());
    puts("PeriPage actual transport: discovery, dimensions, binding, exact packets, stale callbacks and cleanup passed");
}
