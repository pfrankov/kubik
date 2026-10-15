#!/usr/bin/env python3
"""Exercise pinned IDF dynamic RX and stream fetch with the actual WSS BIO."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
idf = Path(os.environ['IDF_PATH'])
dynamic = (idf / 'components/mbedtls/port/dynamic/esp_mbedtls_dynamic_impl.c').read_text()
dynamic = dynamic[dynamic.index('int esp_mbedtls_add_rx_buffer('):dynamic.index('int esp_mbedtls_free_rx_buffer(')]
fetch = (idf / 'components/mbedtls/mbedtls/library/ssl_msg.c').read_text()
fetch = fetch[fetch.index('int mbedtls_ssl_fetch_input('):fetch.index('/*\n * Flush any data not yet written')]
# The real stream loop; DTLS is forbidden by the transport's compile-time guard.
stream = fetch[fetch.index('        while (ssl->in_left < nb_want) {'):fetch.rindex('    MBEDTLS_SSL_DEBUG_MSG(2, ("<= fetch input"));')]
bio = (root / 'firmware/main/link_tls.c').read_text()
bio = bio[bio.index('static int bio_recv('):bio.index('static bool bind_bio(')]
fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MBEDTLS_PRIVATE(x) x
#define MBEDTLS_ERR_SSL_WANT_READ -100
#define ESP_TLS_ERR_SSL_WANT_READ -100
#define MBEDTLS_ERR_SSL_TIMEOUT -101
#define MBEDTLS_ERR_SSL_CONN_EOF -102
#define MBEDTLS_ERR_SSL_INTERNAL_ERROR -103
#define MBEDTLS_ERR_SSL_ALLOC_FAILED -104
#define MBEDTLS_SSL_DEBUG_MSG(a,b) ((void)0)
#define MBEDTLS_SSL_DEBUG_RET(a,b,c) ((void)0)
#define ESP_LOGV(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_MBEDTLS_RETURN_IF_RX_BUF_STATIC(s) ((void)(s))
#define ESP_MBEDTLS_SSL_BUF_CACHED 1
#define SSL_BUF_HEAD_OFFSET_SIZE 0
#define mbedtls_calloc calloc
struct esp_mbedtls_ssl_buf { unsigned char buf[1]; };
typedef int recv_t(void*,unsigned char*,size_t);
typedef struct {
 unsigned char *in_buf,*in_hdr,*in_len,*in_ctr,*in_iv;
 size_t in_left,in_msglen; void *p_bio; recv_t *f_recv;
 int (*f_recv_timeout)(void*,unsigned char*,size_t,uint32_t);
 struct { unsigned read_timeout; } *conf;
} mbedtls_ssl_context;
typedef struct { void *bio; recv_t *recv; unsigned char header[5]; size_t header_used; } tls_link_t;
static int available, forced; static size_t at;
static const unsigned char wire[]={23,3,3,0,3,9,8,7,6,5};
static int raw_recv(void *ctx,unsigned char *out,size_t len) {
 (void)ctx;
 if(forced)return forced==-999?0:forced;
 if(at==(size_t)available)return MBEDTLS_ERR_SSL_WANT_READ;
 if(len>(size_t)available-at)len=available-at;
 memcpy(out,wire+at,len);at+=len;return (int)len;
}
/* BIO */
static int mbedtls_ssl_check_timer(mbedtls_ssl_context *s) { (void)s; return 0; }
static int mbedtls_ssl_fetch_input(mbedtls_ssl_context *ssl,size_t nb_want) {
 int ret;size_t len;
/* STREAM */
 return 0;
}
static int esp_mbedtls_get_buf_state(unsigned char *p) { (void)p; return 0; }
static void esp_mbedtls_free_buf(unsigned char *p) { free(p); }
static size_t mbedtls_ssl_in_hdr_len(mbedtls_ssl_context *s) { (void)s;return 5; }
static void esp_mbedtls_parse_record_header(mbedtls_ssl_context *s) { s->in_msglen=(s->in_len[0]<<8)|s->in_len[1]; }
static int tx_buffer_len(mbedtls_ssl_context *s,size_t n) { (void)s;return (int)n+32; }
static void esp_mbedtls_init_ssl_buf(struct esp_mbedtls_ssl_buf *b,int n) { (void)b;(void)n; }
static void init_rx_buffer(mbedtls_ssl_context *s,unsigned char *b) {
 s->in_buf=b;s->in_ctr=b;s->in_iv=b?b+8:NULL;s->in_hdr=b?b+16:NULL;s->in_len=b?b+19:NULL;
}
/* DYNAMIC */
int main(void) {
 // Contrast: original dynamic wrapper persists a count into an expired stack header.
 mbedtls_ssl_context old={.f_recv=raw_recv}; available=3;
 assert(esp_mbedtls_add_rx_buffer(&old)==MBEDTLS_ERR_SSL_WANT_READ && old.in_left==3);
 // Fixed BIO exposes zero header bytes until all five are available.
 at=0; tls_link_t link={.recv=raw_recv};
 mbedtls_ssl_context ssl={.f_recv=bio_recv,.p_bio=&link};
 assert(esp_mbedtls_add_rx_buffer(&ssl)==MBEDTLS_ERR_SSL_WANT_READ);
 assert(ssl.in_left==0 && link.header_used==3);
 unsigned char scrub[8192]; memset(scrub,0xA5,sizeof scrub);
 available=5;
 assert(!esp_mbedtls_add_rx_buffer(&ssl));
 assert(ssl.in_left==5 && !memcmp(ssl.in_hdr,wire,5) && link.header_used==0);
 free(ssl.in_buf);
 // Five-byte payload reads remain byte-exact too; other lengths delegate.
 unsigned char out[5]; available=8;
 assert(bio_recv(&link,out,5)==MBEDTLS_ERR_SSL_WANT_READ && link.header_used==3);
 available=10; assert(bio_recv(&link,out,5)==5 && !memcmp(out,wire+5,5));
 at=0;available=3;
 assert(bio_recv(&link,out,5)==MBEDTLS_ERR_SSL_WANT_READ);
 assert(bio_recv(&link,out,4)==MBEDTLS_ERR_SSL_INTERNAL_ERROR && !link.header_used);
 at=0;assert(bio_recv(&link,out,5)==MBEDTLS_ERR_SSL_WANT_READ);
 forced=-999;assert(!bio_recv(&link,out,5) && !link.header_used);
 forced=0;at=0;assert(bio_recv(&link,out,5)==MBEDTLS_ERR_SSL_WANT_READ);
 forced=-104;assert(bio_recv(&link,out,5)==-104 && !link.header_used);
 puts("IDF dynamic TLS: original partial-header state reproduced; owned 3+WANT+2 staging and five-byte payload passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-tls-header-') as tmp:
    src, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    # STREAM includes the closing brace of the enclosing stream branch.
    src.write_text(fixture.replace('/* BIO */', bio).replace('/* STREAM */', '{\n' + stream).replace('/* DYNAMIC */', dynamic))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-O1', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
