#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#ifndef ESP_IDF_VERSION
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,1)
#endif
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 1024
#define portMAX_DELAY -1
static void vTaskDelay(int ticks) { struct timespec ts={0,ticks*1000000}; nanosleep(&ts,NULL); }
#define ESP_OK 0
#define ESP_LOGW(tag,fmt,...) do { if (0) fprintf(stderr,fmt,__VA_ARGS__); } while(0)
#define MBEDTLS_ERR_SSL_INTERNAL_ERROR -201
#define ESP_TLS_ERR_SSL_WANT_READ -100
#define ESP_TLS_ERR_SSL_WANT_WRITE -101
#define ERR_TCP_TRANSPORT_CONNECTION_FAILED -2
#define ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN -1
typedef struct { pthread_mutex_t m; pthread_cond_t cv; bool binary; int count; } *SemaphoreHandle_t;
static _Thread_local int locks;
static SemaphoreHandle_t sem(bool binary) {
    SemaphoreHandle_t p=calloc(1,sizeof *p); assert(p);
    pthread_mutex_init(&p->m,NULL); pthread_cond_init(&p->cv,NULL); p->binary=binary; return p;
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return sem(false); }
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { return sem(true); }
static int xSemaphoreTake(SemaphoreHandle_t p,int ticks) {
    if (!p->binary) { pthread_mutex_lock(&p->m); locks++; return 1; }
    pthread_mutex_lock(&p->m);
    if (!ticks && !p->count) { pthread_mutex_unlock(&p->m); return 0; }
    while(!p->count) pthread_cond_wait(&p->cv,&p->m);
    p->count=0; pthread_mutex_unlock(&p->m); return 1;
}
static void xSemaphoreGive(SemaphoreHandle_t p) {
    if (!p->binary) { locks--; pthread_mutex_unlock(&p->m); return; }
    pthread_mutex_lock(&p->m); p->count=1; pthread_cond_broadcast(&p->cv); pthread_mutex_unlock(&p->m);
}
static void vSemaphoreDelete(SemaphoreHandle_t p) {
    pthread_cond_destroy(&p->cv); pthread_mutex_destroy(&p->m); free(p);
}
static int64_t esp_timer_get_time(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (int64_t)ts.tv_sec*1000000+ts.tv_nsec/1000; }
static atomic_int tls_active, destroyed, reads, writes, hold_write, hold_alert;
static int peer, read_wants, write_wants, write_cross, partial, tls_fail, init_fail, connect_fail;
static const void *pending_buffer;
static int pending_len;
#define MBEDTLS_PRIVATE(name) name
typedef int mbedtls_ssl_send_t(void *,const unsigned char *,size_t);
typedef int mbedtls_ssl_recv_t(void *,unsigned char *,size_t);
typedef struct {
    void *p_bio; mbedtls_ssl_send_t *f_send; mbedtls_ssl_recv_t *f_recv;
    void *f_recv_timeout;
} mbedtls_ssl_context;
typedef struct { int fd; mbedtls_ssl_context ssl; } esp_tls_t;
static int socket_send(void *ctx,const unsigned char *b,size_t n) { return (int)send(*(int*)ctx,b,n,0); }
static int socket_recv(void *ctx,unsigned char *b,size_t n) {
    int r=(int)recv(*(int*)ctx,b,n,0);
    return r<0 && errno==EAGAIN ? ESP_TLS_ERR_SSL_WANT_READ : r;
}
static void *esp_tls_get_ssl_context(esp_tls_t *t) { return &t->ssl; }
static void mbedtls_ssl_set_bio(mbedtls_ssl_context *s,void *bio,mbedtls_ssl_send_t *tx,mbedtls_ssl_recv_t *rx,void *timeout) {
    s->p_bio=bio;s->f_send=tx;s->f_recv=rx;s->f_recv_timeout=timeout;
}
typedef struct { int timeout_ms; int (*crt_bundle_attach)(void*); } esp_tls_cfg_t;
typedef void *esp_tls_error_handle_t;
static int link_pin_attach(void *v) { (void)v; return 0; }
static int esp_crt_bundle_attach(void *v) { (void)v; return 0; }
static esp_tls_t *esp_tls_init(void) { return init_fail ? NULL : calloc(1,sizeof(esp_tls_t)); }
static int esp_tls_conn_new_sync(const char *host,int len,int port,const esp_tls_cfg_t *cfg,esp_tls_t *tls) {
    assert(!strcmp(host,"fixture") && len==7 && port==443 && cfg->timeout_ms==100);
    assert(cfg->crt_bundle_attach==link_pin_attach || cfg->crt_bundle_attach==esp_crt_bundle_attach);
    int fds[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds)); tls->fd=fds[0]; peer=fds[1];
    mbedtls_ssl_set_bio(&tls->ssl,&tls->fd,socket_send,socket_recv,NULL);
    return connect_fail ? -1 : 1;
}
static int esp_tls_get_conn_sockfd(esp_tls_t *tls,int *fd) { *fd=tls->fd; return ESP_OK; }
static int esp_tls_get_error_handle(esp_tls_t *tls,void **out) { assert(tls); *out=tls; return ESP_OK; }
static int esp_tls_conn_destroy(esp_tls_t *tls) {
    assert(!atomic_load(&tls_active)); close(tls->fd); free(tls); atomic_fetch_add(&destroyed,1); return 0;
}
static void tls_begin(void) { assert(locks==1 && atomic_fetch_add(&tls_active,1)==0); }
static void tls_end(void) { assert(atomic_fetch_sub(&tls_active,1)==1); }
static int esp_tls_get_bytes_avail(esp_tls_t *tls) { assert(tls); tls_begin(); tls_end(); return 0; }
static int esp_tls_conn_read(esp_tls_t *tls,void *buffer,int n) {
    tls_begin(); atomic_fetch_add(&reads,1);
    if (atomic_load(&hold_alert)) { tls_end(); return ESP_TLS_ERR_SSL_WANT_WRITE; }
    if (read_wants) { char c; assert(recv(tls->fd,&c,1,0)==1); read_wants--; tls_end(); return ESP_TLS_ERR_SSL_WANT_READ; }
    int ret=recv(tls->fd,buffer,n,0); if(ret<0 && errno==EAGAIN)ret=ESP_TLS_ERR_SSL_WANT_READ;
    if(tls_fail)ret=-200;
    tls_end(); return ret;
}
static int esp_tls_conn_write(esp_tls_t *tls,const void *buffer,int n) {
    tls_begin(); atomic_fetch_add(&writes,1);
    assert(n<=CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
    if(pending_buffer) assert(pending_buffer==buffer && pending_len==n);
    if(write_wants || atomic_load(&hold_write)) { pending_buffer=buffer; pending_len=n; if(write_wants)write_wants--; tls_end(); return write_cross?ESP_TLS_ERR_SSL_WANT_READ:ESP_TLS_ERR_SSL_WANT_WRITE; }
    pending_buffer=NULL;
    int ret=send(tls->fd,buffer,partial && n>1?n/2:n,0);
    if(ret<0 && errno==EAGAIN)ret=ESP_TLS_ERR_SSL_WANT_WRITE;
    if(tls_fail)ret=-200;
    tls_end(); return ret;
}
struct transport;
typedef struct transport *esp_transport_handle_t;
struct transport {
    void *data,*foundation;
    int (*_get_socket)(esp_transport_handle_t);
};
static esp_transport_handle_t esp_transport_init(void) { return calloc(1,sizeof(struct transport)); }
static void *esp_transport_get_context_data(esp_transport_handle_t t) { return t->data; }
static void esp_transport_set_context_data(esp_transport_handle_t t,void *s) { t->data=s; }
static void *esp_transport_init_foundation_transport(void) { return malloc(1); }
static void esp_transport_destroy_foundation_transport(void *p) { free(p); }
static void esp_transport_set_errors(esp_transport_handle_t t,void *e) { assert(t->foundation && e); }
static void esp_transport_destroy(esp_transport_handle_t t) { free(t); }
static void esp_transport_set_default_port(esp_transport_handle_t t,int port) { assert(t && port==443); }
static void esp_transport_set_func(esp_transport_handle_t t,
    int(*conn)(esp_transport_handle_t,const char*,int,int), int(*read)(esp_transport_handle_t,char*,int,int),
    int(*write)(esp_transport_handle_t,const char*,int,int),int(*close)(esp_transport_handle_t),
    int(*pr)(esp_transport_handle_t,int),int(*pw)(esp_transport_handle_t,int),int(*destroy)(esp_transport_handle_t)) {
    assert(t&&conn&&read&&write&&close&&pr&&pw&&destroy);
}
static int last_network_error, last_errno, select_error, conservative_writable, select_interrupt;
static void capture_tcp_transport_error(esp_transport_handle_t t,int error) { assert(t->foundation); last_network_error=error; }
static void esp_transport_capture_errno(esp_transport_handle_t t,int error) { assert(t->foundation); last_errno=error; }
typedef struct { const char *ws_path; bool propagate_control_frames; } esp_transport_ws_config_t;
static esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent) { assert(parent); return esp_transport_init(); }
static int esp_transport_ws_set_config(esp_transport_handle_t t,const esp_transport_ws_config_t *c) { assert(t && c->ws_path[0]=='/' && c->propagate_control_frames); return 0; }
static int checked_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *tv) {
    if(select_interrupt) { errno=EINTR; return -1; }
    assert(!locks); if(conservative_writable && w)return 0; if(select_error) { errno=ECONNRESET; return -1; } return select(n,r,w,e,tv);
}
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
static int checked_setsockopt(int fd,int level,int option,const void *data,socklen_t len) {
    assert(fd>=0 && level==IPPROTO_TCP && option==TCP_NODELAY && len==sizeof(int) && *(const int*)data==1); return 0;
}
#define setsockopt checked_setsockopt
#define select checked_select
/* ACTUAL_TRANSPORT */
#undef select
static void nap(void) { struct timespec ts={0,1000000}; nanosleep(&ts,NULL); }
static esp_transport_handle_t connected(bool pin) {
    esp_transport_handle_t t=link_tls_create(pin); assert(t);
    assert(!tls_connect(t,"fixture",443,100));
    assert(fcntl(t->_get_socket(t),F_GETFL,0)&O_NONBLOCK); return t;
}
static void dispose(esp_transport_handle_t t) { tls_destroy(t); esp_transport_destroy(t); close(peer); }
static void *reader(void *p) { char c; intptr_t n=tls_read(p,&c,1,1000); return (void*)n; }
static void *writer(void *p) { return (void *)(intptr_t)tls_write(p,"g",1,1000); }
static void await_write(unsigned before) { for(int i=0;i<1000 && (unsigned)atomic_load(&writes)==before;i++)nap(); assert((unsigned)atomic_load(&writes)>before); }
static void await_read(unsigned before) { for(int i=0;i<1000 && (unsigned)atomic_load(&reads)==before;i++)nap(); assert((unsigned)atomic_load(&reads)>before); }
static void pending_output_tests(void) {
    esp_transport_handle_t t; char b[8]; void *result; pthread_t rx;
    unsigned before;
    // Pending output excludes all other SSL operations until exact-args retry.
    t=connected(false); write_cross=0; atomic_store(&hold_write,1);
    unsigned wc=atomic_load(&writes); before=atomic_load(&reads); pthread_t tx;
    assert(!pthread_create(&tx,NULL,writer,t)); await_write(wc);
    assert(send(peer,"d",1,0)==1); assert(!pthread_create(&rx,NULL,reader,t));
    for(int i=0;i<20;i++)nap(); assert((unsigned)atomic_load(&reads)==before);
    atomic_store(&hold_write,0); pthread_join(tx,&result); assert((intptr_t)result==1);
    pthread_join(rx,&result); assert((intptr_t)result==1); dispose(t);
    t=connected(false); atomic_store(&hold_alert,1); before=atomic_load(&reads); wc=atomic_load(&writes);
    assert(send(peer,"e",1,0)==1); assert(!pthread_create(&rx,NULL,reader,t)); await_read(before);
    assert(!pthread_create(&tx,NULL,writer,t)); for(int i=0;i<20;i++)nap();
    assert((unsigned)atomic_load(&writes)==wc); atomic_store(&hold_alert,0);
    pthread_join(rx,&result); assert((intptr_t)result==1); pthread_join(tx,&result); assert((intptr_t)result==1); dispose(t);
    t=connected(false); atomic_store(&hold_write,1);
    assert(tls_write(t,"z",1,20)==ERR_TCP_TRANSPORT_CONNECTION_FAILED);
    atomic_store(&hold_write,0); pending_buffer=NULL;
    assert(((tls_link_t*)t->data)->closing && !((tls_link_t*)t->data)->pending);
    assert(tls_read(t,b,1,10)==ERR_TCP_TRANSPORT_CONNECTION_FAILED); dispose(t);
}
static void low_water_test(esp_transport_handle_t t) {
    char b[8];
    conservative_writable=1; write_wants=1;
    assert(poll_write(t,100)==1 && tls_write(t,"low",3,100)==3);
    assert(recv(peer,b,3,0)==3 && !memcmp(b,"low",3)); conservative_writable=0;
}
static void fragmented_header_test(esp_transport_handle_t t) {
    char b[8];
    tls_link_t *owned=t->data; unsigned char header[5];
    assert(send(peer,"abc",3,0)==3);
    assert(bio_recv(owned,header,5)==ESP_TLS_ERR_SSL_WANT_READ && owned->header_used==3);
    memset(header,0,sizeof header);
    assert(bio_send(owned,(const unsigned char*)"h",1)==1 && recv(peer,b,1,0)==1);
    assert(send(peer,"de",2,0)==2);
    assert(bio_recv(owned,header,5)==5 && !memcmp(header,"abcde",5) && !owned->header_used);
}
static void large_write_test(esp_transport_handle_t t) {
    char large[4096], received[4096]; memset(large, 'L', sizeof large);
    assert(tls_write(t,large,sizeof large,100)==sizeof large);
    assert(recv(peer,received,sizeof received,MSG_WAITALL)==sizeof received && !memcmp(large,received,sizeof large));
}
static void interrupted_wait_test(esp_transport_handle_t t) {
    char byte;
    select_interrupt=1;
    int64_t start=esp_timer_get_time();
    assert(tls_read(t,&byte,1,30)==0);
    assert(esp_timer_get_time()-start>=25000 && esp_timer_get_time()-start<100000);
    select_interrupt=0;
}
int main(void) {
    esp_transport_handle_t t=connected(false); char b[8];
    int64_t start=esp_timer_get_time(); assert(tls_read(t,b,1,30)==0 && last_network_error==0);
    assert(esp_timer_get_time()-start>=25000 && esp_timer_get_time()-start<100000);
    interrupted_wait_test(t);
    select_error=1; assert(tls_read(t,b,1,30)==-2 && last_network_error==-2 && last_errno==ECONNRESET); select_error=0;
    fragmented_header_test(t);
    low_water_test(t);
    write_wants=3; assert(tls_write(t,"hello",5,100)==5); assert(!pending_buffer);
    assert(recv(peer,b,5,0)==5 && !memcmp(b,"hello",5));
    write_cross=1; write_wants=1; assert(send(peer,"r",1,0)==1);
    assert(tls_write(t,"x",1,100)==1 && !pending_buffer); assert(tls_read(t,b,1,100)==1 && b[0]=='r');
    assert(recv(peer,b,1,0)==1 && b[0]=='x');
    large_write_test(t);
    partial=1; assert(tls_write(t,"four",4,100)==4); partial=0;
    tls_fail=1; assert(tls_write(t,"x",1,100)==-200); tls_fail=0;
    close(peer); peer=-1; assert(tls_read(t,b,1,100)==ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN && last_network_error==-1);
    assert(!tls_close(t)); assert(poll_read(t,0)==-1 && tls_write(t,"x",1,100)==-2);
    int count=atomic_load(&destroyed); assert(!tls_close(t) && atomic_load(&destroyed)==count); dispose(t);
    t=connected(true); read_wants=1; unsigned before=atomic_load(&reads);
    assert(send(peer,"a",1,0)==1); pthread_t rx; assert(!pthread_create(&rx,NULL,reader,t)); await_read(before);
    // RX waits for the rest of a TLS record; TX must progress concurrently.
    assert(tls_write(t,"tx",2,100)==2); assert(recv(peer,b,2,0)==2 && !memcmp(b,"tx",2));
    assert(send(peer,"b",1,0)==1); void *result; pthread_join(rx,&result); assert((intptr_t)result==1);
    before=atomic_load(&reads); read_wants=1; assert(send(peer,"c",1,0)==1);
    assert(!pthread_create(&rx,NULL,reader,t)); await_read(before);
    assert(!tls_close(t)); pthread_join(rx,&result); assert((intptr_t)result<0);
    assert(((tls_link_t*)t->data)->active==0); dispose(t);
    pending_output_tests();
    t=link_tls_create(false); init_fail=1; assert(tls_connect(t,"fixture",443,100)<0); init_fail=0; dispose(t);
    t=link_tls_create(false); connect_fail=1; assert(tls_connect(t,"fixture",443,100)<0); connect_fail=0; dispose(t);
    puts("TLS: WANT retries, cross-direction wait, absolute timeout, partial/EOF/error, RX/TX concurrency, close lease drain and ownership passed");
}
