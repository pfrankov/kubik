#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <assert.h>
#include <stdio.h>

static esp_bt_controller_status_t status=ESP_BT_CONTROLLER_STATUS_INITED;
static size_t available,largest;
static unsigned free_probes,largest_probes,status_probes,real_calls;
static esp_bt_mode_t last_mode;
static esp_err_t real_result;
esp_bt_controller_status_t esp_bt_controller_get_status(void){status_probes++;return status;}
size_t heap_caps_get_free_size(unsigned caps){assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));free_probes++;return available;}
size_t heap_caps_get_largest_free_block(unsigned caps){assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));largest_probes++;return largest;}
esp_err_t __real_esp_bt_controller_enable(esp_bt_mode_t mode){real_calls++;last_mode=mode;return real_result;}
esp_err_t __wrap_esp_bt_controller_enable(esp_bt_mode_t mode);

static void rejected(size_t free_bytes,size_t largest_bytes) {
    available=free_bytes;largest=largest_bytes;unsigned before=real_calls;
    unsigned free_before=free_probes,largest_before=largest_probes;
    assert(__wrap_esp_bt_controller_enable(ESP_BT_MODE_BLE)==ESP_ERR_NO_MEM);
    assert(real_calls==before && free_probes==free_before+1 && largest_probes==largest_before+1);
}
static void valid_boundaries(void) {
    const size_t stack=CONFIG_BT_LE_CONTROLLER_TASK_STACK_SIZE,required=stack+sizeof(StaticTask_t)+4096+512;
    assert(stack==4096 && sizeof(StaticTask_t)==336 && required==9040);
    rejected(2776,1344); // Actual Wi-Fi Live controller-task allocation failure.
    rejected(required,stack-1);rejected(required-1,stack);rejected(100000,512);
    available=required;largest=stack;unsigned before=real_calls;real_result=ESP_OK;
    assert(__wrap_esp_bt_controller_enable(ESP_BT_MODE_BLE)==ESP_OK && real_calls==before+1);
    assert(last_mode==ESP_BT_MODE_BLE);
    available=required+10000;largest=stack+1000;real_result=-77;
    assert(__wrap_esp_bt_controller_enable(ESP_BT_MODE_BLE)==-77);
    rejected(2776,1344); // Every call reads current heap; success is never cached.
}
static void vendor_validation(void) {
    const esp_bt_mode_t modes[]={ESP_BT_MODE_IDLE,ESP_BT_MODE_CLASSIC_BT,ESP_BT_MODE_BTDM,(esp_bt_mode_t)99};
    real_result=-91;
    for(unsigned i=0;i<sizeof modes/sizeof *modes;i++) {
        unsigned before=real_calls,probes=free_probes+largest_probes;
        assert(__wrap_esp_bt_controller_enable(modes[i])==-91 && real_calls==before+1);
        assert(last_mode==modes[i] && probes==free_probes+largest_probes);
    }
    const esp_bt_controller_status_t states[]={ESP_BT_CONTROLLER_STATUS_IDLE,ESP_BT_CONTROLLER_STATUS_ENABLED,ESP_BT_CONTROLLER_STATUS_NUM};
    for(unsigned i=0;i<sizeof states/sizeof *states;i++) {
        status=states[i];unsigned probes=free_probes+largest_probes,before=real_calls;
        assert(__wrap_esp_bt_controller_enable(ESP_BT_MODE_BLE)==-91 && real_calls==before+1);
        assert(probes==free_probes+largest_probes);
    }
    status=ESP_BT_CONTROLLER_STATUS_INITED;rejected(2776,1344);
}
int main(void) {
    valid_boundaries();vendor_validation();assert(status_probes && free_probes==largest_probes);
    puts("BLE controller guard: recorded low heap, exact thresholds, fragmentation, fresh probes and vendor errors passed");
}
