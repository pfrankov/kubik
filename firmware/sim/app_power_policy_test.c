#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "app_state.h"
#include "power_network.h"
static int s_talk, s_gen=-1, s_srv, s_quiet_reads=LIGHT_SLEEP_QUIET_READS;
static power_state_t s_power=PWR_DARK;
static bool radio, online, host, fast, doze, cpu_sleep, audio_sleep;
#define s_online online
#define TALK_IDLE 0
#define SS_IDLE 0
static int phase=POWER_NETWORK_ACTIVE;
static int64_t s_debug_network_until;
static int64_t now_ms(void) {return 1000;}
static int app_network_phase(void) {return phase;}
static bool wifi_radio_started(void) {return radio;}
static bool link_usb_host_present(void) {return host;}
static void wifi_set_fast(bool value) {fast=value;}
static void wifi_set_doze(bool value) {doze=value;}
static void audio_doze(bool value) {audio_sleep=value;}
static void power_light_sleep_allow(bool value) {cpu_sleep=value;}
/* PRODUCTION_POWER */
int main(void) {
 host=true;update_power_saving();assert(!cpu_sleep&&audio_sleep);
 s_quiet_reads=0;s_debug_network_until=2000;update_power_saving();assert(cpu_sleep&&audio_sleep);
 phase=POWER_NETWORK_CHECK;update_power_saving();assert(fast&&!cpu_sleep);
 radio=true;update_power_saving();assert(fast&&!cpu_sleep&&audio_sleep);
 phase=POWER_NETWORK_ACTIVE;online=true;update_power_saving();assert(!fast&&!cpu_sleep&&doze);
 online=false;update_power_saving();assert(fast&&!cpu_sleep);
 radio=false;update_power_saving();assert(!fast&&cpu_sleep);
 host=false;s_quiet_reads=0;update_power_saving();assert(!cpu_sleep);
 s_debug_network_until=0;update_power_saving();assert(!cpu_sleep);
 s_quiet_reads=LIGHT_SLEEP_QUIET_READS;s_power=PWR_AWAKE;update_power_saving();assert(!cpu_sleep&&!audio_sleep);
 s_talk=1;update_power_saving();assert(fast&&!cpu_sleep);
 return 0;
}
