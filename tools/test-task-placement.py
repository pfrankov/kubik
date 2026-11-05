#!/usr/bin/env python3
"""Compile production core policy and execute every owned firmware task-creation call."""
from pathlib import Path
import ast
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware/main'
# Preserve every argument except the new, explicit core affinity.
EXPECTED = {
    ('app.c', 'app'): ('app_task', 'app_stack', 'NULL', '6', '&s_app_task', 'CONFIG_IDF_TARGET_ESP32S3 ? 8192 : 4096', '6'),
    ('app.c', 'power'): ('power_task', '3072', 'NULL', '2', 'NULL', '3072', '2'),
    ('audio.c', 'spk'): ('speaker_task', '4096', 'NULL', '10', '&s_spk_task', '4096', '10'),
    ('audio.c', 'mic'): ('mic_capture_task', '4096', '(void *)&s_mic_task_config', '6', '&s_mic_task', '4096', '6'),
    ('input.c', 'input'): ('input_task', '4096', 'NULL', '9', '&s_input_task', '4096', '9'),
    ('link.c', 'link'): ('manager_task', '5632', 'NULL', '4', 'NULL', '5632', '4'),
    ('link_usb.c', 'usb_rx'): ('usb_rx_task', '3584', 'NULL', '8', 'NULL', '3584', '8'),
    ('main.c', 'display'): ('display_task', 'display_stack', 'NULL', '8', '&s_display_task', 'CONFIG_IDF_TARGET_ESP32S3 ? 9216 : 7168', '8'),
    ('muse_backend.c', 'muse'): ('worker', '7168', 'NULL', '4', 'NULL', '7168', '4'),
    ('muse_ble.c', 'muse_pair'): ('worker', '6144', 'NULL', '4', 'NULL', '6144', '4'),
    ('setup_dns.c', 'dns'): ('dns_task', '3072', 'NULL', '3', 'NULL', '3072', '3'),
}
STATIC = ('mic_delivery.c', 'mic_send')
SPI = ('display_s3_tx.c', 's3_spi')
CORE_ZERO = {'spk', 'mic', 'mic_send', 's3_spi'}
TOKENS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*', re.S)
CREATOR = re.compile(r'\b(xTaskCreate\w*)\s*\(')


def code_only(text):
    return TOKENS.sub(lambda match: ' ' if match[0].startswith('/') else match[0], text)


def arguments(text, start):
    depth, quote, escaped, current, result = 1, '', False, '', []
    for character in text[start:]:
        if quote:
            current += character
            if character == quote and not escaped:
                quote = ''
            escaped = character == '\\' and not escaped
            continue
        if character in ('"', "'"):
            quote = character
        elif character == '(':
            depth += 1
        elif character == ')':
            depth -= 1
            if not depth:
                return result + [current.strip()]
        elif character == ',' and depth == 1:
            result.append(current.strip());current = '';continue
        current += character
    raise AssertionError('Unclosed task-creation call')


def compact(expression):
    return re.sub(r'\s+', '', expression)


def inventory():
    found = []
    for path in sorted(MAIN.glob('*.c')):
        source = code_only(path.read_text())
        for match in CREATOR.finditer(source):
            args = arguments(source, match.end())
            key = (path.name, ast.literal_eval(args[1]))
            found.append((key, match[1], args))
    keys = [key for key, _, _ in found]
    assert len(keys) == len(set(keys)) and set(keys) == set(EXPECTED) | {STATIC, SPI}, keys
    return found


def validate(found):
    for key, creator, args in found:
        role = 'KUBIK_AUDIO_CORE' if key[1] in CORE_ZERO else 'KUBIK_UI_CORE'
        if key == SPI:
            prefix = ['sender', '"s3_spi"', 'TX_STACK_BYTES', 'NULL', 'TX_PRIORITY', 's_stack', '&s_task_storage']
            expected_creator = 'xTaskCreateStaticPinnedToCore'
        elif key == STATIC:
            prefix = ['delivery_task', '"mic_send"', 'duplex ? 3072 : STACK_BYTES', 'NULL', 'duplex ? 6 : 7', 's_stack', '&s_task_storage']
            expected_creator = 'xTaskCreateStaticPinnedToCore'
        else:
            callback, stack, context, priority, handle, _, _ = EXPECTED[key]
            prefix = [callback, f'"{key[1]}"', stack, context, priority, handle]
            expected_creator = 'xTaskCreatePinnedToCore'
        assert creator == expected_creator, (key, creator)
        assert list(map(compact, args)) == list(map(compact, [*prefix, role])), (key, args)


def stack_declaration(filename, variable):
    source = code_only((MAIN / filename).read_text())
    at = source.index(f'const unsigned {variable}')
    start, end = source.rfind('#if', 0, at), source.index('#endif', at) + len('#endif')
    block = source[start:end]
    assert '#if CONFIG_IDF_TARGET_ESP32S3' in block and block.count(f'const unsigned {variable}') == 2
    return block


def s3_module_boundary():
    cmake = (MAIN / 'CMakeLists.txt').read_text()
    block = re.search(r'if\(IDF_TARGET STREQUAL "esp32s3"\)(.*?)\nendif\(\)', cmake, re.S)
    assert block and cmake.count(SPI[0]) == 1 and SPI[0] in block[1], 'SPI worker must be S3-only'
    before = block[1].split(SPI[0], 1)[0]
    assert not re.search(r'\b(?:else|elseif)\s*\(', before), 'SPI worker moved outside the S3 branch'


def spi_storage():
    source = code_only((MAIN / SPI[0]).read_text())
    size = re.search(r'^#define TX_STACK_BYTES\s+\d+\s*$', source, re.M)
    priority = re.search(r'^#define TX_PRIORITY[^\n]+$', source, re.M)
    scheduler = re.search(r'_Static_assert\(configMAX_PRIORITIES\b.*?;', source, re.S)
    stack = re.search(r'^static StackType_t s_stack\[TX_STACK_BYTES / sizeof\(StackType_t\)\];$', source, re.M)
    storage = re.search(r'^static StaticTask_t s_task_storage;$', source, re.M)
    assert size and compact(size[0]) == '#defineTX_STACK_BYTES4096'
    assert priority and compact(priority[0]) == '#defineTX_PRIORITY(configMAX_PRIORITIES-2)'
    assert scheduler and compact(scheduler[0]).startswith('_Static_assert(configMAX_PRIORITIES==25&&configUSE_TIME_SLICING==1,')
    assert stack and storage, 'SPI sender must retain its independent static stack and TCB'
    return conditional_spi('\n'.join([size[0], priority[0], scheduler[0]])), '\n'.join([stack[0], storage[0]])


def conditional_spi(code):
    return '#if CONFIG_IDF_TARGET_ESP32S3\n' + code + '\n#endif'


def harness(found):
    fixture = (ROOT / 'tools/fixtures/task_placement_test.c').read_text()
    delivery = code_only((MAIN / STATIC[0]).read_text())
    stack_bytes = re.search(r'^#define STACK_BYTES\s+\d+\s*$', delivery, re.M)
    assert stack_bytes and compact(stack_bytes[0]) == '#defineSTACK_BYTES4096'
    spi_size, spi_memory = spi_storage()
    declarations = '\n'.join([stack_bytes[0], spi_size, stack_declaration('app.c', 'app_stack'), stack_declaration('main.c', 'display_stack')])
    callbacks = sorted({args[0] for key, _, args in found if key != SPI})
    definitions = '\n'.join(f'static void {name}(void *arg) {{(void)arg;}}' for name in callbacks)
    definitions += '\n' + conditional_spi('static void sender(void *arg) {(void)arg;}')
    creations, expected = [], []
    for key, creator, args in found:
        call = '(void)' + creator + '(' + ','.join(args) + ');'
        role = '0' if key[1] in CORE_ZERO else 'EXPECTED_UI_CORE'
        if key == SPI:
            call = conditional_spi('{\n' + spi_memory + '\nsender_memory=s_stack;sender_storage=&s_task_storage;\n' + call + '\n}')
            # A priority-8 sender starved behind the ready SDK Wi-Fi task (23),
            # exhausting START's 40 ms deadline. Peer priority requires tick slicing.
            stack, priority, fixed = '4096', '23', 'true'
        elif key == STATIC:
            stack, priority, fixed = 'duplex ? 3072 : 4096', 'duplex ? 6 : 7', 'true'
        else:
            *_, stack, priority = EXPECTED[key]
            fixed = 'false'
        creations.append(call)
        entry = f'{{"{key[1]}",{stack},{priority},{role},{fixed}}},'
        expected.append(conditional_spi(entry) if key == SPI else entry)
    return (fixture.replace('/* PRODUCTION_STACK_SIZES */', declarations).replace('/* TASK_CALLBACKS */', definitions)
            .replace('/* PRODUCTION_CREATIONS */', '\n'.join(creations)).replace('/* EXPECTED_RECORDS */', '\n' + '\n'.join(expected)))


def run():
    s3_module_boundary();found = inventory();validate(found)
    with tempfile.TemporaryDirectory(prefix='kubik-task-placement-') as temporary:
        directory = Path(temporary)
        source, executable = directory / 'test.c', directory / 'test'
        source.write_text(harness(found))
        cases = ((0, 1, 25, 1, None), (0, 0, 25, 1, None), (0, 1, 24, 0, None),
                 (1, 0, 25, 1, None), (1, 1, 25, 1, r'(S3|ESP32S3).*?(UNICORE|dual|cores)'),
                 (1, 0, 24, 1, r'S3 sender requires.*time slicing'),
                 (1, 0, 25, 0, r'S3 sender requires.*time slicing'))
        for s3, unicore, priorities, slicing, diagnostic in cases:
            (directory / 'sdkconfig.h').write_text(f'#define CONFIG_IDF_TARGET_ESP32S3 {s3}\n#define CONFIG_IDF_TARGET_ESP32C6 {1-s3}\n#define CONFIG_FREERTOS_UNICORE {unicore}\n'
                                                  f'#define configMAX_PRIORITIES {priorities}\n#define configUSE_TIME_SLICING {slicing}\n')
            result = subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-DESP_PLATFORM=1', f'-DEXPECTED_UI_CORE={s3}',
                                     '-I', str(directory), '-I', str(MAIN), str(source), '-o', str(executable)],
                                    text=True, capture_output=True)
            if diagnostic:
                assert result.returncode and re.search(diagnostic, result.stderr, re.I), result.stderr
            else:
                assert not result.returncode, result.stderr
                subprocess.run([str(executable)], check=True)
    print('task placement: C6 exactly 12 creators; S3 13 with static 4096-byte priority-23 SPI sender on core zero; SDK 25/time-slicing and unicore guards preserved')


if __name__ == '__main__':
    run()
