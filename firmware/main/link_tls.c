// WSS-only full duplex: TLS calls are serialized; socket waits never hold TLS.
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 1)
#error "Review the pinned transport socket/error ABI before an IDF upgrade"
#endif
#if defined(CONFIG_MBEDTLS_SSL_PROTO_TLS1_3) || defined(CONFIG_MBEDTLS_SSL_RENEGOTIATION) || defined(CONFIG_MBEDTLS_SSL_PROTO_DTLS)
#error "Duplex TLS interleaving requires the audited TLS 1.2 stream profile"
#endif
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "mbedtls/ssl.h"
#include "esp_transport_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_transport_ws.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "link_pin.h"
#include "link_tls.h"

typedef struct {
    esp_tls_t *tls;
    const void *pending;
    SemaphoreHandle_t io, idle;
    unsigned active;
    int fd;
    bool closing, pinned;
    void *bio;
    mbedtls_ssl_send_t *send;
    mbedtls_ssl_recv_t *recv;
    unsigned char header[5];
    size_t header_used;
} tls_link_t;

static int bio_send(void *ctx, const unsigned char *data, size_t len) {
    tls_link_t *s = ctx;
    return s->send(s->bio, data, len);
}

static int bio_recv(void *ctx, unsigned char *data, size_t len) {
    tls_link_t *s = ctx;
    if (len != sizeof s->header) {
        if (s->header_used) { s->header_used = 0; return MBEDTLS_ERR_SSL_INTERNAL_ERROR; }
        return s->recv(s->bio, data, len);
    }
    // IDF's dynamic RX wrapper stages the five-byte TLS header on its stack.
    // Never expose a partial header across WANT; keep it owned by this link.
    while (s->header_used < sizeof s->header) {
        int n = s->recv(s->bio, s->header + s->header_used, sizeof s->header - s->header_used);
        if (n <= 0) {
            if (n != ESP_TLS_ERR_SSL_WANT_READ) s->header_used = 0;
            return n;
        }
        s->header_used += n;
    }
    memcpy(data, s->header, sizeof s->header);
    s->header_used = 0;
    return sizeof s->header;
}

static bool bind_bio(tls_link_t *s) {
    mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(s->tls);
    if (!ssl || !ssl->MBEDTLS_PRIVATE(f_send) || !ssl->MBEDTLS_PRIVATE(f_recv) ||
        ssl->MBEDTLS_PRIVATE(f_recv_timeout)) return false;
    s->bio = ssl->MBEDTLS_PRIVATE(p_bio);
    s->send = ssl->MBEDTLS_PRIVATE(f_send); s->recv = ssl->MBEDTLS_PRIVATE(f_recv);
    s->header_used = 0;
    mbedtls_ssl_set_bio(ssl, s, bio_send, bio_recv, NULL);
    return true;
}

static int64_t deadline(int ms) {
    // Link operations are bounded even if a caller asks to wait forever.
    return esp_timer_get_time() + (int64_t)(ms < 0 ? 15000 : ms) * 1000;
}

static bool enter(tls_link_t *s) {
    xSemaphoreTake(s->io, portMAX_DELAY);
    bool ok = s->tls && s->fd >= 0 && !s->closing;
    if (ok && !s->active++) xSemaphoreTake(s->idle, 0);
    xSemaphoreGive(s->io);
    return ok;
}

static void leave(tls_link_t *s) {
    xSemaphoreTake(s->io, portMAX_DELAY);
    if (!--s->active) xSemaphoreGive(s->idle);
    xSemaphoreGive(s->io);
}

static int select_once(int fd, bool writing, int64_t end) {
    int64_t us = end - esp_timer_get_time();
    if (us < 0) us = 0;
    if (us > 100000) us = 100000;
    struct timeval tv = { .tv_sec = 0, .tv_usec = us };
    fd_set fds, errors;
    FD_ZERO(&fds); FD_ZERO(&errors); FD_SET(fd, &fds); FD_SET(fd, &errors);
    int n = select(fd + 1, writing ? NULL : &fds, writing ? &fds : NULL, &errors, &tv);
    if (n < 0 && errno == EINTR) return -3;
    if (n < 0 || (n > 0 && FD_ISSET(fd, &errors))) return -1;
    return n;
}

static int ready(tls_link_t *s, bool writing, int64_t end) {
    for (;;) {
        xSemaphoreTake(s->io, portMAX_DELAY);
        int fd = s->closing ? -1 : s->fd;
        int cached = 0;
        if (!writing && fd >= 0 && !s->pending) cached = esp_tls_get_bytes_avail(s->tls);
        xSemaphoreGive(s->io);
        if (fd < 0) return -1;
        if (cached > 0) return 1;
        int n = select_once(fd, writing, end);
        if (n == -3) {
            if (esp_timer_get_time() >= end) return 0;
            continue;
        }
        if (n) return n;
        if (esp_timer_get_time() >= end) return 0;
    }
}

static void capture(esp_transport_handle_t t, tls_link_t *s) {
    esp_tls_error_handle_t errors;
    if (esp_tls_get_error_handle(s->tls, &errors) == ESP_OK) esp_transport_set_errors(t, errors);
}

static void network_error(esp_transport_handle_t t, tls_link_t *s, int code) {
    int saved_errno = errno;
    xSemaphoreTake(s->io, portMAX_DELAY);
    if (code == ERR_TCP_TRANSPORT_CONNECTION_FAILED && s->fd >= 0) {
        int socket_error = 0;
        socklen_t size = sizeof socket_error;
        if (!getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) && socket_error)
            saved_errno = socket_error;
        esp_transport_capture_errno(t, saved_errno);
    }
    capture_tcp_transport_error(t, code);
    xSemaphoreGive(s->io);
}

#define IO_BUSY (-30001)
static bool wants_io(int n) { return n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE; }

static int call_tls(esp_transport_handle_t t, void *data, int len, bool writing, const void *owner) {
    tls_link_t *s = esp_transport_get_context_data(t);
    int64_t begin = esp_timer_get_time();
    xSemaphoreTake(s->io, portMAX_DELAY);
    int64_t owned = esp_timer_get_time();
    int n = ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    if (!s->closing) {
        if (s->pending && s->pending != owner) n = IO_BUSY;
        else {
            // Keep ESP-TLS from hiding WANT after a successful earlier fragment.
            int chunk = len > CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN ? CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN : len;
            n = writing ? esp_tls_conn_write(s->tls, data, chunk) : esp_tls_conn_read(s->tls, data, len);
            // A pending output record must resume before any other TLS call.
            bool exclusive = wants_io(n) && (writing || n == ESP_TLS_ERR_SSL_WANT_WRITE);
            s->pending = exclusive ? owner : NULL;
            if (n < 0 && !wants_io(n)) capture(t, s);
        }
    }
    xSemaphoreGive(s->io);
    int64_t end = esp_timer_get_time();
    if (end - begin > 80000) ESP_LOGW("wss_tls", "slow call: lock=%d op=%d ms write=%d result=%d",
        (int)((owned-begin)/1000), (int)((end-owned)/1000), writing, n);
    return n;
}

static int finish_transfer(esp_transport_handle_t t, const void *owner, int n, bool writing) {
    tls_link_t *s = esp_transport_get_context_data(t);
    xSemaphoreTake(s->io, portMAX_DELAY);
    if (s->pending == owner) {
        // Never reuse a half-written TLS record after a deadline or socket error.
        s->pending = NULL; s->closing = true; shutdown(s->fd, SHUT_RDWR);
        n = ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    }
    xSemaphoreGive(s->io);
    if (writing && n == 0) n = ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    if (n <= 0) network_error(t, s, n);
    leave(s);
    return n;
}

static int transfer_result(int n, int offset, int len, bool writing) {
    if (writing && offset == len) return len;
    if (writing && offset) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    return wants_io(n) || n == IO_BUSY ? 0 : n;
}

static int io_ready(tls_link_t *s, bool writing, int64_t end, bool first) {
    if (!writing) return ready(s, false, end);
    if (first) return 1;
    if (esp_timer_get_time() >= end) return 0;
    xSemaphoreTake(s->io, portMAX_DELAY);
    int fd = s->closing ? -1 : s->fd;
    xSemaphoreGive(s->io);
    if (fd < 0) return -1;
    int64_t slice = esp_timer_get_time() + 10000;
    if (slice > end) slice = end;
    int n = select_once(fd, true, slice);
    // POLLOUT requires > half the TCP buffer, even when our short record fits.
    // After a bounded wait retry the actual nonblocking write below that mark.
    return n >= 0 || n == -3 ? 1 : -1;
}

// Report waits outside the TLS mutex too; a fast SSL call can still follow
// socket backpressure or an exclusive retry owned by another operation.
static void report_transfer(int64_t started, int len, bool writing, unsigned busy,
                            unsigned want_read, unsigned want_write, int result) {
    int64_t elapsed = esp_timer_get_time() - started;
    if (writing && elapsed > 80000) ESP_LOGW("wss_tls", "transfer=%dms bytes=%d busy=%u want_read=%u want_write=%u last_tls_result=%d",
        (int)(elapsed / 1000), len, busy, want_read, want_write, result);
}

static int transfer(esp_transport_handle_t t, void *data, int len, int ms, bool writing) {
    tls_link_t *s = esp_transport_get_context_data(t);
    if (!enter(s)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    int64_t end = deadline(ms);
    int64_t started = esp_timer_get_time();
    unsigned busy = 0, want_read = 0, want_write = 0;
    int n = 0, offset = 0;
    bool wait_write = writing, first = true;
    do {
        n = io_ready(s, wait_write, end, first);
        first = false;
        if (n < 0) { n = ERR_TCP_TRANSPORT_CONNECTION_FAILED; break; }
        if (!n) break;
        n = call_tls(t, (char *)data + offset, len - offset, writing, &offset);
        busy += n == IO_BUSY; want_read += n == ESP_TLS_ERR_SSL_WANT_READ; want_write += n == ESP_TLS_ERR_SSL_WANT_WRITE;
        if (n == IO_BUSY) { vTaskDelay(1); continue; }
        if (wants_io(n)) { wait_write = n == ESP_TLS_ERR_SSL_WANT_WRITE; continue; }
        if (!writing) { if (!n) n = ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN; break; }
        if (n <= 0) break;
        offset += n;
        if (offset == len) break;
        wait_write = true;
    } while (esp_timer_get_time() < end);
    report_transfer(started, len, writing, busy, want_read, want_write, n);
    n = transfer_result(n, offset, len, writing);
    return finish_transfer(t, &offset, n, writing);
}

static int tls_read(esp_transport_handle_t t, char *b, int n, int ms) { return transfer(t, b, n, ms, false); }
static int tls_write(esp_transport_handle_t t, const char *b, int n, int ms) { return transfer(t, (void *)b, n, ms, true); }

static int poll_read(esp_transport_handle_t t, int ms) {
    tls_link_t *s = esp_transport_get_context_data(t);
    if (!enter(s)) return -1;
    int n = ready(s, false, deadline(ms));
    if (n <= 0) network_error(t, s, n < 0 ? ERR_TCP_TRANSPORT_CONNECTION_FAILED : 0);
    leave(s);
    return n;
}
static int poll_write(esp_transport_handle_t t, int ms) {
    (void)ms;
    tls_link_t *s = esp_transport_get_context_data(t);
    if (!enter(s)) return -1;
    // TLS owns a bounded output record. Its write callback performs the actual
    // nonblocking socket attempt and deadline; SDK preflight must not wait for
    // lwIP's larger select low-water mark while holding the sender lock.
    leave(s);
    return 1;
}

static int tls_close(esp_transport_handle_t t) {
    tls_link_t *s = esp_transport_get_context_data(t);
    xSemaphoreTake(s->io, portMAX_DELAY);
    s->closing = true;
    // Wake pollers without freeing/reusing their fd or TLS context.
    if (s->fd >= 0) shutdown(s->fd, SHUT_RDWR);
    xSemaphoreGive(s->io);
    xSemaphoreTake(s->idle, portMAX_DELAY);
    xSemaphoreTake(s->io, portMAX_DELAY);
    int n = s->tls ? esp_tls_conn_destroy(s->tls) : 0;
    s->tls = NULL; s->fd = -1; s->header_used = 0;
    xSemaphoreGive(s->io);
    xSemaphoreGive(s->idle);
    return n;
}

static int tls_connect(esp_transport_handle_t t, const char *host, int port, int ms) {
    tls_link_t *s = esp_transport_get_context_data(t);
    tls_close(t);
    // SDK connects before publishing CONNECTED: no concurrent admission here.
    esp_tls_cfg_t cfg = { .timeout_ms = ms,
        .crt_bundle_attach = s->pinned ? link_pin_attach : esp_crt_bundle_attach };
    s->tls = esp_tls_init();
    if (!s->tls) return -1;
    if (esp_tls_conn_new_sync(host, strlen(host), port, &cfg, s->tls) != 1) {
        capture(t, s); tls_close(t); return -1;
    }
    int fd, flags, nodelay = 1;
    if (esp_tls_get_conn_sockfd(s->tls, &fd) != ESP_OK ||
        (flags = fcntl(fd, F_GETFL, 0)) < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay) < 0) {
        tls_close(t); return -1;
    }
    if (!bind_bio(s)) { tls_close(t); return -1; }
    xSemaphoreTake(s->io, portMAX_DELAY);
    s->fd = fd; s->closing = false;
    xSemaphoreGive(s->io);
    return 0;
}

static int tls_socket(esp_transport_handle_t t) {
    tls_link_t *s = esp_transport_get_context_data(t);
    xSemaphoreTake(s->io, portMAX_DELAY);
    int fd = s->closing ? -1 : s->fd;
    xSemaphoreGive(s->io);
    return fd;
}

static int tls_destroy(esp_transport_handle_t t) {
    tls_link_t *s = esp_transport_get_context_data(t);
    if (s) {
        tls_close(t);
        vSemaphoreDelete(s->io); vSemaphoreDelete(s->idle); free(s);
    }
    esp_transport_destroy_foundation_transport(t->foundation);
    return 0;
}

esp_transport_handle_t link_tls_create(bool pinned) {
    tls_link_t *s = calloc(1, sizeof *s);
    esp_transport_handle_t t = esp_transport_init();
    if (s) { s->io = xSemaphoreCreateMutex(); s->idle = xSemaphoreCreateBinary(); }
    if (!s || !t || !s->io || !s->idle) {
        if (s) {
            if (s->io) vSemaphoreDelete(s->io);
            if (s->idle) vSemaphoreDelete(s->idle);
            free(s);
        }
        if (t) esp_transport_destroy(t);
        return NULL;
    }
    t->foundation = esp_transport_init_foundation_transport();
    if (!t->foundation) {
        vSemaphoreDelete(s->io); vSemaphoreDelete(s->idle); free(s); esp_transport_destroy(t); return NULL;
    }
    s->fd = -1; s->closing = true; s->pinned = pinned;
    xSemaphoreGive(s->idle);
    esp_transport_set_context_data(t, s);
    esp_transport_set_func(t, tls_connect, tls_read, tls_write, tls_close, poll_read, poll_write, tls_destroy);
    t->_get_socket = tls_socket;
    esp_transport_set_default_port(t, 443);
    return t;
}

esp_transport_handle_t link_tls_wss(const char *uri, bool pinned, esp_transport_handle_t *parent) {
    *parent = link_tls_create(pinned);
    esp_transport_handle_t ws = *parent ? esp_transport_ws_init(*parent) : NULL;
    const char *path = strchr(uri + 6, '/');
    esp_transport_ws_config_t config = { .ws_path = path ? path : "/", .propagate_control_frames = true };
    if (ws && esp_transport_ws_set_config(ws, &config) != ESP_OK) { esp_transport_destroy(ws); ws = NULL; }
    if (ws) esp_transport_set_default_port(ws, 443);
    if (!ws && *parent) { esp_transport_destroy(*parent); *parent = NULL; }
    return ws;
}
