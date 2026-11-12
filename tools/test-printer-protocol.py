#!/usr/bin/env python3
"""Check the production printer RPC boundary with the real application RX/tick hooks."""
from pathlib import Path
import os
import json
import importlib.util
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf-v5.5.1"))
CJSON = IDF / "components/json/cJSON"


def function(source, start, following):
    text = (ROOT / source).read_text()
    return text[text.index(start):text.index(following, text.index(start))]


with tempfile.TemporaryDirectory(prefix="kubik-printer-") as temporary:
    directory = Path(temporary)
    fixture = (ROOT / "firmware/sim/printer_protocol_test.c").read_text()
    fixture = fixture.replace("/* PRODUCTION_ON_JSON */", function(
        "firmware/main/app_protocol.c", "static void on_json(", "static void on_audio("))
    fixture = fixture.replace("/* PRODUCTION_APP_TICK */", function(
        "firmware/main/app_runtime.c", "void app_tick(void)", "static void setup_result_face("))
    source = directory / "printer.c"
    source.write_text(fixture)
    sources = ("printer_protocol", "printer_parse", "printer_image_parse", "printer_stream", "emoji_cluster", "printer_emoji", "printer_emoji_parse", "printer_work", "printer_job", "printer_result", "printer_guard",
               "printer_layout", "printer_layout_text", "printer_layout_qr")
    qr = ROOT / "firmware/managed_components/espressif__qrcode"
    for target in ("ESP32C6", "ESP32S3"):
        binary = directory / target.lower()
        work = directory / f"work-{target}.o"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined", "-Ifirmware/main",
            "-Ifirmware/sim/display_s3_stubs", "-Ifirmware/sim/settings_stubs", f"-I{CJSON}", "-Dcalloc=printer_test_calloc", "-c", "firmware/main/printer_work.c",
            "-o", str(work)], cwd=ROOT, check=True)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f"-DCONFIG_IDF_TARGET_{target}=1", "-Ifirmware/main", "-Ifirmware/sim", "-Ifirmware/sim/settings_stubs",
            "-Ifirmware/sim/display_s3_stubs", f"-I{CJSON}", str(source),
            *[f"firmware/main/{name}.c" for name in sources if name != "printer_work"], str(work),
            "firmware/main/link_hello.c", "firmware/main/status_json.c", "firmware/main/font.c", "firmware/main/font_data.c",
            f"-I{qr}", str(qr / "qrcodegen.c"), str(CJSON / "cJSON.c"), "-lm", "-o", str(binary),
        ], cwd=ROOT, check=True)
        output = subprocess.check_output([str(binary)], cwd=ROOT, text=True, timeout=20)
        print(output, end='')
        messages = [json.loads(line.removeprefix('PIPELINE_WIRE ')) for line in output.splitlines() if line.startswith('PIPELINE_WIRE ')]
        subprocess.run(['node', '--input-type=module', '-e',
            "import {parseDeviceMessage} from './openclaw-kubik/src/protocol.js';"
            "let input='';for await(const part of process.stdin)input+=part;"
            "for(const message of JSON.parse(input))parseDeviceMessage(Buffer.from(JSON.stringify(message)));"],
            input=json.dumps(messages), text=True, cwd=ROOT, check=True, timeout=10)
        spec = importlib.util.spec_from_file_location('printer_contract', ROOT / 'hermes-kubik/printer_contract.py')
        contract = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(contract)
        for message in messages:
            assert contract.result(message, 'print' if message['status'] == 'sent' else 'status')
    voice = (ROOT / 'firmware/sim/printer_voice_admission_test.c').read_text()
    voice = voice.replace('/* PRODUCTION_PTT_DOWN */', function(
        'firmware/main/app_control.c', 'void ptt_down(void)', 'void ptt_up(void)'))
    voice = voice.replace('/* PRODUCTION_PRINTER_STATE */', function(
        'firmware/main/app_runtime.c', 'static uint32_t printer_blockers(', 'void app_tick(void)'))
    voice_source = directory / 'voice.c'
    voice_source.write_text(voice)
    voice_binary = directory / 'voice'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-Ifirmware/main', str(voice_source), 'firmware/main/app_state.c',
        '-o', str(voice_binary)], cwd=ROOT, check=True)
    subprocess.run([str(voice_binary)], cwd=ROOT, check=True, timeout=10)
    guard = (ROOT / 'firmware/sim/printer_guard_test.c').read_text()
    system_header = (IDF / 'components/esp_system/include/esp_system.h').read_text()
    reset_enum = system_header[system_header.index('typedef enum {'):system_header.index('} esp_reset_reason_t;')+len('} esp_reset_reason_t;')]
    guard = guard.replace('/* SDK_RESET_ENUM */', reset_enum).replace('/* PRODUCTION_RESET_CLASSIFIER */', function(
        'firmware/main/printer_platform.c', 'static bool maintenance_reset(', 'void printer_platform_boot('))
    guard_source = directory / 'guard.c'
    guard_source.write_text(guard)
    guard_binary = directory / 'guard'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-Ifirmware/main', str(guard_source), 'firmware/main/printer_guard.c',
        '-o', str(guard_binary)], cwd=ROOT, check=True)
    subprocess.run([str(guard_binary)], cwd=ROOT, check=True, timeout=10)
    visibility = (ROOT / 'firmware/sim/printer_visibility_test.c').read_text().replace('/* PRODUCTION_VISIBILITY */', function(
        'firmware/main/app_menu.c', 'bool app_printer_visible(', 'void menu_sync('))
    visibility_source = directory / 'visibility.c'
    visibility_source.write_text(visibility)
    visibility_binary = directory / 'visibility'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-Ifirmware/main', str(visibility_source), 'firmware/main/app_state.c',
        '-o', str(visibility_binary)], cwd=ROOT, check=True)
    subprocess.run([str(visibility_binary)], cwd=ROOT, check=True, timeout=10)
    stream_binary = directory / "stream"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
        "-pthread", "-fsanitize=address,undefined", "-Ifirmware/main", "-Ifirmware/sim/display_s3_stubs",
        "firmware/main/printer_stream.c", "firmware/sim/printer_stream_test.c", "-o", str(stream_binary)],
        cwd=ROOT, check=True)
    subprocess.run([str(stream_binary)], cwd=ROOT, check=True, timeout=10)
print("Printer RPC v2: both targets passed ASan/UBSan using production parser, jobs, renderer, hello and app tick")
