// Host test of LAN-mode trust: the TLS verify callback and USB binding (link_pin.c)
// and finding the plugin (link_discover.c), with the real mbedTLS SHA-256 and faked sockets.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "mbedtls/ssl.h"
#include "settings.h"

settings_t g_settings;
static int pin_saves;
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test"; }
esp_err_t settings_save_pin(const uint8_t pin[SETTINGS_PIN_BYTES]) {
    memcpy(g_settings.server_pin, pin, SETTINGS_PIN_BYTES);
    g_settings.server_pinned = true;
    pin_saves++;
    return ESP_OK;
}

// mbedtls_ssl_conf_* only record what the hook installs.
static const mbedtls_x509_crt *installed_ca;
static int (*installed_verify)(void *, mbedtls_x509_crt *, int, uint32_t *);
void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca, mbedtls_x509_crl *crl) {
    (void)conf; (void)crl; installed_ca = ca;
}
void mbedtls_ssl_conf_verify(mbedtls_ssl_config *conf, int (*f)(void *, mbedtls_x509_crt *, int, uint32_t *), void *p) {
    (void)conf; (void)p; installed_verify = f;
}

// Sockets: one fake UDP socket with a queue of replies.
typedef struct { char data[200]; int len; const char *from; } datagram_t;
static datagram_t replies[4];
static int nreplies, sends, closes, broadcast_on;
static char sent[64];
static struct sockaddr_in sent_to;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { (void)key; return NULL; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info) { (void)netif; (void)info; return ESP_FAIL; }
static int fake_socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; return 7; }
static int fake_setsockopt(int s, int level, int name, const void *value, socklen_t len) {
    (void)s; (void)len;
    if (level == SOL_SOCKET && name == SO_BROADCAST) broadcast_on = *(const int *)value;
    return 0;
}
static int fake_bind(int s, const struct sockaddr *addr, socklen_t len) { (void)s; (void)addr; (void)len; return 0; }
static int fake_close(int s) { assert(s == 7); closes++; return 0; }
static ssize_t fake_sendto(int s, const void *data, size_t len, int flags, const struct sockaddr *to, socklen_t to_len) {
    (void)s; (void)flags; (void)to_len;
    snprintf(sent, sizeof sent, "%.*s", (int)len, (const char *)data);
    memcpy(&sent_to, to, sizeof sent_to);
    sends++;
    return (ssize_t)len;
}
static ssize_t fake_recvfrom(int s, void *data, size_t cap, int flags, struct sockaddr *from, socklen_t *from_len) {
    (void)s; (void)flags;
    if (!nreplies) return -1;
    datagram_t d = replies[0];
    memmove(replies, replies + 1, sizeof replies - sizeof replies[0]);
    nreplies--;
    struct sockaddr_in in = {.sin_family = AF_INET};
    inet_pton(AF_INET, d.from, &in.sin_addr);
    memcpy(from, &in, sizeof in);
    *from_len = sizeof in;
    size_t n = (size_t)d.len < cap ? (size_t)d.len : cap;
    memcpy(data, d.data, n);
    return (ssize_t)n;
}
#define socket fake_socket
#define setsockopt fake_setsockopt
#define bind fake_bind
#define close fake_close
#define sendto fake_sendto
#define recvfrom fake_recvfrom
#define TAG discover_tag
#include "../main/link_discover.c"
#undef TAG
#undef bind
#include "../main/link_pin.c"

// SHA-256("abc"): the leaf's SubjectPublicKeyInfo in these tests.
static const char ABC[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
static const char OTHER[] = "0000000000000000000000000000000000000000000000000000000000000001";

static int handshake(const char *spki, uint32_t *leaf_flags) {
    mbedtls_x509_crt leaf = {0}, issuer = {0};
    leaf.pk_raw.p = (unsigned char *)spki;
    leaf.pk_raw.len = strlen(spki);
    uint32_t issuer_flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    *leaf_flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH;
    // mbedTLS walks from the top of the chain down to the leaf.
    assert(installed_verify(NULL, &issuer, 1, &issuer_flags) == 0 && issuer_flags == 0);
    return installed_verify(NULL, &leaf, 0, leaf_flags);
}

static void test_modes(void) {
    assert(link_server_mode("") == SERVER_LAN_DISCOVER);
    assert(link_server_mode("kubik://mac.local") == SERVER_LAN_FIXED);
    assert(link_server_mode("wss://x/kubik/v1") == SERVER_CA);
    assert(link_server_mode("ws://x/kubik/v1") == SERVER_INVALID);
    strcpy(g_settings.server_url, "ws://x/kubik/v1");
    char rejected[LINK_BIND_MAX];
    assert(!link_pin_bind(false, 0, rejected));
    char bind[LINK_BIND_MAX];
    strcpy(g_settings.server_url, "wss://x/kubik/v1");
    assert(link_pin_bind(false, 0, bind) && !strcmp(bind, "ca:x"));
    // The host as the plugin compares it: lowercase, no port, no IPv6 brackets, nothing of the path.
    const char *urls[][2] = {{"wss://Kubik.Example.COM/kubik/v1", "ca:kubik.example.com"},
                             {"wss://gw.example.com:8443/kubik/v1", "ca:gw.example.com"},
                             {"wss://100.64.0.5:443/a/b:c", "ca:100.64.0.5"},
                             {"wss://[FD00::1]:8443/kubik/v1", "ca:fd00::1"},
                             {"wss://[fd00::1]/kubik/v1", "ca:fd00::1"}};
    for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++) {
        strcpy(g_settings.server_url, urls[i][0]);
        assert(link_pin_bind(false, 0, bind) && !strcmp(bind, urls[i][1]));
    }
    strcpy(g_settings.server_url, "ws://x/kubik/v1");
    assert(!link_pin_bind(false, 0, bind));
    link_pin_keep(false, 0);
    assert(!pin_saves);  // only LAN mode pins
}

static void test_wifi_pinning(void) {
    char bind[LINK_BIND_MAX];
    uint32_t flags;
    strcpy(g_settings.server_url, "");
    assert(link_pin_attach(NULL) == ESP_OK && installed_ca && installed_verify);
    link_pin_begin_wifi();
    assert(!link_pin_bind(false, 0, bind));  // no handshake yet: nothing to sign
    // First use: any key is accepted and remembered for this connection only.
    assert(handshake("abc", &flags) == 0 && flags == 0);
    assert(link_pin_bind(false, 0, bind) && !strcmp(bind, ABC));
    assert(!g_settings.server_pinned && !link_pin_take_mismatch());
    link_pin_keep(false, 0);  // after the welcome
    assert(g_settings.server_pinned && pin_saves == 1);
    link_pin_keep(false, 0);
    assert(pin_saves == 1);
    // The same key again is trusted; another one fails the handshake and is reported once.
    link_pin_begin_wifi();
    assert(handshake("abc", &flags) == 0 && flags == 0);
    link_pin_begin_wifi();
    assert(handshake("abd", &flags) == 0 && flags != 0);
    assert(!link_pin_bind(false, 0, bind));
    assert(link_pin_take_mismatch() && !link_pin_take_mismatch());
    assert(pin_saves == 1);  // never re-pinned automatically
}

static void test_usb_binding(void) {
    char bind[LINK_BIND_MAX];
    strcpy(g_settings.server_url, "kubik://mac.local");  // pinned to ABC by the previous test
    link_pin_usb_bind(5, OTHER, strlen(OTHER));
    assert(!link_pin_bind(true, 5, bind) && link_pin_take_mismatch());
    link_pin_usb_bind(5, ABC, strlen(ABC));
    assert(link_pin_bind(true, 5, bind) && !strcmp(bind, ABC) && !link_pin_bind(true, 6, bind));
    const char *malformed[] = {"ca:x", "ca", "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "ba78"};
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        link_pin_usb_bind(5, malformed[i], strlen(malformed[i]));
        assert(!link_pin_bind(true, 5, bind) && !link_pin_take_mismatch());
    }
    // No pin: trusted on first use, kept only by link_pin_keep after the welcome.
    g_settings.server_pinned = false;
    link_pin_usb_bind(9, OTHER, strlen(OTHER));
    assert(link_pin_bind(true, 9, bind) && !strcmp(bind, OTHER));
    link_pin_keep(true, 8);
    assert(!g_settings.server_pinned);
    link_pin_keep(true, 9);
    assert(g_settings.server_pinned && g_settings.server_pin[31] == 1 && !g_settings.server_pin[0]);
    // CA and development modes sign what the bridge reports, but never odd characters.
    strcpy(g_settings.server_url, "wss://x/kubik/v1");
    link_pin_usb_bind(10, "ca:gw.example.com", 17);
    assert(link_pin_bind(true, 10, bind) && !strcmp(bind, "ca:gw.example.com"));
    link_pin_usb_bind(10, "ca:fd00::1", 10);
    assert(link_pin_bind(true, 10, bind) && !strcmp(bind, "ca:fd00::1"));
    link_pin_usb_bind(10, "ca\nx", 4);
    assert(!link_pin_bind(true, 10, bind));
    link_pin_usb_bind(10, "ca:x y", 6);
    assert(!link_pin_bind(true, 10, bind));
    link_pin_usb_bind(10, "ca:Gw", 5);
    assert(!link_pin_bind(true, 10, bind));
}

static void reply(const char *text, const char *from) {
    datagram_t *d = &replies[nreplies++];
    d->len = snprintf(d->data, sizeof d->data, "%s", text);
    d->from = from;
}

static void test_targets(void) {
    char uri[160];
    const char *fixed_cases[][2] = {
        {"kubik://mac.local", "wss://mac.local:18790/kubik/v1"},
        {"kubik://10.0.0.2:9000", "wss://10.0.0.2:9000/kubik/v1"},
        {"kubik://[fe80::1]", "wss://[fe80::1]:18790/kubik/v1"},
        {"kubik://[fe80::1]:9", "wss://[fe80::1]:9/kubik/v1"},
        {"wss://x.example/kubik/v1", "wss://x.example/kubik/v1"},
    };
    for (size_t i = 0; i < sizeof fixed_cases / sizeof fixed_cases[0]; i++) {
        strcpy(g_settings.server_url, fixed_cases[i][0]);
        assert(link_target(0, uri, sizeof uri) == TARGET_READY && !strcmp(uri, fixed_cases[i][1]));
    }
    assert(!sends);

    // Nobody answers: three broadcasts a second apart, then "not found".
    strcpy(g_settings.server_url, "");
    assert(link_target(100, uri, sizeof uri) == TARGET_WAITING && sends == 1 && broadcast_on);
    assert(!strcmp(sent, "kubik-discover-v5") && sent_to.sin_addr.s_addr == htonl(INADDR_BROADCAST) &&
           ntohs(sent_to.sin_port) == 18790);
    assert(link_target(900, uri, sizeof uri) == TARGET_WAITING && sends == 1);
    assert(link_target(1100, uri, sizeof uri) == TARGET_WAITING && sends == 2);
    assert(link_target(2100, uri, sizeof uri) == TARGET_WAITING && sends == 3);
    assert(link_target(3100, uri, sizeof uri) == TARGET_NOT_FOUND && closes == 1);

    // Wrong or oversized replies are ignored; the plugin is where its reply came from.
    assert(link_target(5000, uri, sizeof uri) == TARGET_WAITING && sends == 4);
    char big[160];
    snprintf(big, sizeof big, "{\"t\":\"kubik\",\"v\":5,\"port\":18790,\"pad\":\"%0120d\"}", 0);
    reply(big, "192.168.1.6");
    reply("{\"t\":\"kubik\",\"v\":3,\"port\":18790}", "192.168.1.8");
    reply("{\"t\":\"kubik\",\"v\":5,\"port\":0}", "192.168.1.8");
    reply("{\"t\":\"kubik\",\"v\":5,\"port\":18791}", "192.168.1.7");
    assert(link_target(5200, uri, sizeof uri) == TARGET_READY && !strcmp(uri, "wss://192.168.1.7:18791/kubik/v1"));
    assert(closes == 2);
    link_target_stop();
    assert(closes == 2);
}

int main(void) {
    test_modes();
    test_wifi_pinning();
    test_usb_binding();
    test_targets();
    puts("pin: TOFU after welcome, pinned match/mismatch without re-pin, USB bind checks, CA host binding and rejected plaintext address, "
         "kubik:// targets and LAN discovery passed");
}
