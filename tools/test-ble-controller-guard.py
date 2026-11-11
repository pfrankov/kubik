#!/usr/bin/env python3
"""Exercise the actual C6 controller-enable wrapper without modifying ESP-IDF."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='kubik-ble-guard-') as temporary:
    directory=Path(temporary)
    (directory/'freertos').mkdir()
    (directory/'sdkconfig.h').write_text('#define CONFIG_BT_LE_CONTROLLER_TASK_STACK_SIZE 4096\n')
    (directory/'esp_bt.h').write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
typedef enum {ESP_BT_MODE_IDLE,ESP_BT_MODE_BLE,ESP_BT_MODE_CLASSIC_BT,ESP_BT_MODE_BTDM} esp_bt_mode_t;
typedef enum {ESP_BT_CONTROLLER_STATUS_IDLE,ESP_BT_CONTROLLER_STATUS_INITED,ESP_BT_CONTROLLER_STATUS_ENABLED,ESP_BT_CONTROLLER_STATUS_NUM} esp_bt_controller_status_t;
esp_bt_controller_status_t esp_bt_controller_get_status(void);
''')
    (directory/'esp_heap_caps.h').write_text('''#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_8BIT 2u
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
''')
    (directory/'esp_log.h').write_text('#define ESP_LOGW(...) ((void)0)\n')
    (directory/'freertos/FreeRTOS.h').write_text('#pragma once\n')
    (directory/'freertos/task.h').write_text('#pragma once\ntypedef struct {unsigned char bytes[336];} StaticTask_t;\n')
    binary=directory/'guard'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',f'-I{directory}',
        'firmware/main/ble_controller_guard.c','tools/fixtures/ble_controller_guard_test.c','-o',str(binary)],cwd=ROOT,check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
cmake=(ROOT/'firmware/main/CMakeLists.txt').read_text()
block='''if(IDF_TARGET STREQUAL "esp32c6")
    target_sources(${COMPONENT_LIB} PRIVATE ble_controller_guard.c)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_bt_controller_enable,--undefined=__wrap_esp_bt_controller_enable")
endif()'''
assert block in cmake, 'C6 alone must link the guard and force the wrapper into the firmware'
