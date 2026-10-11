#!/usr/bin/env python3
"""Run setup DNS task ownership and bounded stop/reopen against native socket mocks."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#define ESP_LOGE(...) ((void)0)
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
static pthread_t thread;
static bool created, fail_task, fail_socket, fail_bind, fail_timeout;
static atomic_int sockets, closes, reads, tasks;
static void vTaskDelay(int ms) { usleep(ms * 1000); }
static void vTaskDelete(void *unused) { (void)unused; }
static void (*task_entry)(void *);
static void *launch(void *unused) { task_entry(NULL); return NULL; }
static int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                       void *arg, int priority, void *out) {
    assert(!strcmp(name, "dns") && stack == 3072 && !arg && !out && priority == 3);
    if (fail_task) return 0;
    assert(!created);
    task_entry = entry; created = true;
    atomic_fetch_add(&tasks, 1);
    assert(!pthread_create(&thread, NULL, launch, NULL));
    return pdPASS;
}
static void join_task(void) {
    if (created) { assert(!pthread_join(thread, NULL)); created = false; }
}
static int test_socket(int domain, int type, int protocol) {
    assert(domain == AF_INET && type == SOCK_DGRAM && protocol == IPPROTO_UDP);
    if (fail_socket) return -1;
    atomic_fetch_add(&sockets, 1); return 99;
}
static int test_bind(int fd, const struct sockaddr *address, socklen_t length) {
    assert(fd == 99 && length == sizeof(struct sockaddr_in));
    const struct sockaddr_in *ip = (const void *)address;
    const uint8_t bytes[4] = {192,168,4,1};
    assert(ip->sin_port == htons(53) && !memcmp(&ip->sin_addr.s_addr, bytes, 4));
    return fail_bind ? -1 : 0;
}
static int test_setsockopt(int fd, int level, int option, const void *value, socklen_t size) {
    assert(fd == 99 && level == SOL_SOCKET && option == SO_RCVTIMEO && size == sizeof(struct timeval));
    const struct timeval *timeout = value;
    assert(!timeout->tv_sec && timeout->tv_usec == 400000);
    return fail_timeout ? -1 : 0;
}
static ssize_t test_recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *from, socklen_t *length) {
    assert(fd == 99); atomic_fetch_add(&reads, 1); usleep(30000); return -1;
}
static int test_close(int fd) { assert(fd == 99); atomic_fetch_add(&closes, 1); return 0; }
#define socket test_socket
#define bind test_bind
#define setsockopt test_setsockopt
#define recvfrom test_recvfrom
#define close test_close
'''
CHECKS = r'''
static void stopped(void) {
    setup_dns_stop(); join_task();
    assert(!atomic_load(&s_active) && !atomic_load(&s_running));
    assert(atomic_load(&sockets) == atomic_load(&closes));
}
int main(void) {
    stopped();
    for (int cycle = 0; cycle < 20; cycle++) {
        int before_tasks = atomic_load(&tasks), before_reads = atomic_load(&reads);
        setup_dns_start(); setup_dns_start(); // duplicate start never owns a second socket
        assert(atomic_load(&tasks) == before_tasks + 1);
        while (atomic_load(&reads) == before_reads) usleep(1000);
        stopped(); // immediate reopen must find the old socket released
    }
    fail_task = true; setup_dns_start(); assert(!s_active); stopped(); fail_task = false;
    fail_socket = true; setup_dns_start(); join_task(); stopped(); fail_socket = false;
    fail_bind = true; setup_dns_start(); join_task(); stopped(); fail_bind = false;
    fail_timeout = true; setup_dns_start(); join_task(); stopped(); fail_timeout = false;
    setup_dns_start(); stopped(); // failures must not poison a later session
    puts("setup DNS: AP-only binding, single owner, 20 stop/reopen cycles and allocation/socket failures passed");
}
'''

with tempfile.TemporaryDirectory(prefix='kubik-dns-test-') as directory:
    source = Path(directory) / 'dns.c'
    binary = Path(directory) / 'dns'
    body = re.sub(r'^#include.*\n', '', (ROOT / 'firmware/main/setup_dns.c').read_text(), flags=re.M)
    source.write_text(MOCKS + body + CHECKS)
    subprocess.run(['clang', '-std=c11', '-D_DEFAULT_SOURCE', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wno-unused-variable',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
