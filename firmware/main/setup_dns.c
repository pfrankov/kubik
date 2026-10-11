#include "setup_dns.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "setup";
static atomic_bool s_running, s_active;

// DNS: every name is Kubik. The task owns its socket until it has closed it.
static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53)};
    const uint8_t ip[4] = {192, 168, 4, 1};
    memcpy(&addr.sin_addr.s_addr, ip, 4);  // only the setup network, never the home LAN
    struct timeval tv = {.tv_sec = 0, .tv_usec = 400000};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        ESP_LOGE(TAG, "dns socket failed");
    } else {
        uint8_t buf[300];
        while (atomic_load(&s_running)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
            if (n < 12 || (buf[2] & 0x80)) continue;  // not a query
            // Walk the single question to find its type.
            int p = 12;
            while (p < n && buf[p]) p += buf[p] + 1;
            if (p + 5 > n) continue;
            int qtype = (buf[p + 1] << 8) | buf[p + 2];
            int end = p + 5;
            buf[2] = 0x81; buf[3] = 0x80;  // response, recursion available, no error
            buf[6] = 0; buf[7] = 0; buf[8] = buf[9] = buf[10] = buf[11] = 0;
            if (qtype == 1) {  // A: this device
                buf[7] = 1;
                const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, ip[0], ip[1], ip[2], ip[3]};
                memcpy(buf + end, ans, sizeof ans);
                end += sizeof ans;
            }
            sendto(sock, buf, end, 0, (struct sockaddr *)&from, fl);
        }
    }
    if (sock >= 0) close(sock);
    atomic_store(&s_active, false);
    vTaskDelete(NULL);
}

void setup_dns_start(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_active, &expected, true)) {
        ESP_LOGE(TAG, "dns task already active");
        return;
    }
    atomic_store(&s_running, true);
    if (xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL) != pdPASS) {
        atomic_store(&s_running, false);
        atomic_store(&s_active, false);
        ESP_LOGE(TAG, "dns task failed");
    }
}

void setup_dns_stop(void) {
    atomic_store(&s_running, false);
    // A receive expires within 400 ms. Bound teardown even if the network stack misbehaves;
    // start refuses a second task until the previous socket is actually released.
    for (int i = 0; i < 100 && atomic_load(&s_active); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (atomic_load(&s_active)) ESP_LOGE(TAG, "dns stop timed out");
}
