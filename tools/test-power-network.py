#!/usr/bin/env python3
"""Exercise the pure scheduled Wi-Fi radio-off coordinator on the host."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix="kubik-power-network-") as tmp:
    executable = Path(tmp) / "power-network-test"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
        "-fsanitize=address,undefined", "-Ifirmware/main", "firmware/sim/power_network_test.c",
        "firmware/main/power_network.c", "-o", str(executable),
    ], cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)

# Exercise the actual PMIC reader with status/ADC failures; hardware I2C is the only stub.
board = (ROOT / 'firmware/main/board.c').read_text()
reader = board[board.index('void pmic_read('):board.index('int pmic_pwr_keys(')]
header = (ROOT / 'firmware/main/board.h').read_text()
status = header[header.index('typedef struct {'):header.index('void pmic_read(')]
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
static int registers[256];
static int axp_read(uint8_t reg) { return registers[reg]; }
'''
test = r'''
int main(void) {
    power_status_t power;
    registers[0] = 0x28; registers[1] = 0; registers[0xA4] = 100;
    pmic_read(&power);
    assert(power.power_known && power.usb_power && !power.charging && power.battery_pct == 100);
    registers[1] = 0x20; registers[0xA4] = 60;
    pmic_read(&power); assert(power.usb_power && power.charging);
    registers[0] = 0x08; registers[1] = 0x40;
    pmic_read(&power); assert(power.power_known && !power.usb_power && !power.charging);
    for (int status = 0; status < 2; status++) {
        registers[0] = registers[1] = 0; registers[status] = -1;
        pmic_read(&power);
        assert(!power.power_known && power.usb_power && !power.charging && power.battery_pct == -1);
    }
    registers[0] = 0x20; registers[1] = 0;
    pmic_read(&power); assert(power.power_known && power.usb_power && !power.battery_present && power.battery_pct == -1);
    registers[0] = 0x28; registers[0xA4] = -1;
    pmic_read(&power); assert(power.power_known && power.usb_power && power.battery_pct == -1);
    registers[0xA4] = 255;
    pmic_read(&power); assert(power.power_known && power.usb_power && power.battery_pct == -1);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-pmic-') as tmp:
    source, executable = Path(tmp) / 'pmic.c', Path(tmp) / 'pmic'
    source.write_text(stub + status + reader + test)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PMIC: charging, full, unplugged, missing battery and failed status/percentage reads passed')

# The production power policy must exercise the unplugged CPU gate as well as radio-off.
runtime = (ROOT / 'firmware/main/app_runtime.c').read_text()
policy = runtime[runtime.index('static void update_power_saving('):runtime.index('static void status_tick(')]
with tempfile.TemporaryDirectory(prefix='kubik-power-policy-') as tmp:
    source = Path(tmp) / 'policy.c'
    source.write_text((ROOT / 'firmware/sim/app_power_policy_test.c').read_text().replace('/* PRODUCTION_POWER */', policy))
    executable = Path(tmp) / 'policy'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-Ifirmware/main', str(source), 'firmware/main/app_state.c', '-o', str(executable)], cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)
print('power policy: USB/battery diagnostic parity, radio-off CPU sleep, awake network checks/joins and idle modem sleep passed')
