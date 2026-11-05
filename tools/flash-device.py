#!/usr/bin/env python3
"""Verify a Kubik release bundle and flash its four images over USB."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from firmware_layout import TARGETS, validate_image_chip


def validate_target_character(manifest):
    chip = manifest.get("chip")
    if not isinstance(chip, str) or chip not in TARGETS or manifest.get("flash_size") != "16MB":
        raise ValueError("This bundle is not for a supported 16 MB Kubik target")
    board = manifest.get('board')
    if board != TARGETS[chip]['board']:
        raise ValueError('Bundle board profile does not match its manifest chip')
    if manifest.get("character") not in {"Tess", "Plush"}:
        raise ValueError("Firmware character is missing or invalid")
    return chip, board, 0x320000 if manifest['character'] == 'Tess' else 0x200000


def validate_manifest_parts(parts, assets_offset):
    if any(not isinstance(part, dict) or type(part.get('offset')) is not int
           or not isinstance(part.get('file'), str) for part in parts):
        raise ValueError("Invalid flash image entry")
    expected = {(0, 'bootloader.bin'), (0x8000, 'partition-table.bin'),
                (0x20000, 'kubik.bin'), (assets_offset, 'assets.bin')}
    if {(part.get('offset'), part.get('file')) for part in parts} != expected:
        raise ValueError("Unexpected flash layout")


def validate_manifest(manifest):
    if not isinstance(manifest, dict):
        raise ValueError("Invalid firmware manifest")
    chip, board, assets_offset = validate_target_character(manifest)
    parts = manifest.get('parts')
    if not isinstance(parts, list) or len(parts) != 4:
        raise ValueError("Expected bootloader, partition table, app and assets")
    validate_manifest_parts(parts, assets_offset)
    return chip, board, sorted(parts, key=lambda part: part["offset"])


def bundle_files(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    chip, _, ordered = validate_manifest(manifest)
    validate_images(directory, ordered, chip)
    return manifest


def validate_images(directory, ordered, chip):
    for index, part in enumerate(ordered):
        name = part.get("file")
        if not isinstance(name, str) or Path(name).name != name:
            raise ValueError("Invalid image name")
        data = (directory / name).read_bytes()
        if len(data) != part.get("size") or hashlib.sha256(data).hexdigest() != part.get("sha256"):
            raise ValueError(f"Image failed integrity check: {name}")
        if name in {'bootloader.bin', 'kubik.bin'}:
            validate_image_chip(data, chip, name)
        if part["offset"] + len(data) > 0x1000000:
            raise ValueError(f"Image exceeds the device flash: {name}")
        if index + 1 < len(ordered) and part["offset"] + len(data) > ordered[index + 1]["offset"]:
            raise ValueError(f"Flash images overlap: {name}")


def select_port(explicit):
    if explicit:
        return explicit
    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("Install esptool first: python3 -m pip install esptool==4.13.0") from error
    ports = [port.device for port in list_ports.comports() if port.vid == 0x303A]
    if len(ports) != 1:
        raise RuntimeError(f"Expected one connected Espressif USB device, found {len(ports)}; pass --port")
    return ports[0]


def bridge_uses_port(command, port):
    """Match a node bridge's actual --port argument, never a port mentioned inside another command."""
    import shlex
    try:
        argv = shlex.split(command)
    except ValueError:
        return False
    if not argv or Path(argv[0]).name != "node":
        return False
    if len(argv) < 2 or not argv[1].endswith("/usb-bridge/bridge.mjs"):
        return False
    return any(arg == "--port" and n + 1 < len(argv) and argv[n + 1] == port
               for n, arg in enumerate(argv))


def running_bridges(port):
    """Only USB bridges holding the selected port can race this flash."""
    try:
        found = subprocess.run(["ps", "-axo", "pid=,args="], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    pids = []
    for line in found.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and parts[0].isdigit() and bridge_uses_port(parts[1], port):
            if int(parts[0]) != os.getpid():
                pids.append(int(parts[0]))
    return pids


def stop_bridges(port):
    """Stops running USB bridges before flashing: a bridge that opens the port while esptool resets the chip can leave the chip stuck until the USB cable is replugged."""
    pids = running_bridges(port)
    for pid in pids:
        os.kill(pid, signal.SIGTERM)
    if pids:
        time.sleep(1)
        print(f"USB-мост остановлен (pid {', '.join(map(str, pids))}). После прошивки подождите 8 с, "
              "убедитесь, что устройство загрузилось, и только потом запустите мост: "
              "node tools/usb-bridge/bridge.mjs --port <порт>")
    return bool(pids)


def flash_images(directory, manifest, port):
    s3 = manifest['chip'] == 'esp32s3'
    tool = [sys.executable, "-m", "esptool", "--chip", manifest['chip'], "--port", port]
    command = tool + ["--baud", "460800", "--before", "usb_reset" if s3 else "default_reset",
                      "--after", "no_reset" if s3 else "hard_reset", "write_flash",
                      "--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"]
    for part in sorted(manifest["parts"], key=lambda part: part["offset"]):
        command.extend([hex(part["offset"]), str(directory / part["file"])])
    stop_bridges(port)
    subprocess.run(command, check=True)
    if s3:
        # ESP32-S3 native USB can retain forced download mode after an RTS reset.
        # Clear only RTC_FORCE_DOWNLOAD_BOOT, then restart through the RTC WDT.
        reset = tool + ["--before", "no_reset", "--after", "watchdog_reset",
                        "write_mem", "0x6000812c", "0", "0x1"]
        try:
            subprocess.run(reset, check=True, timeout=30)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            raise RuntimeError("Образы записаны, но перезапуск ESP32-S3 не завершён; "
                               "проверьте USB-порт; восстановление загрузки: "
                               "https://github.com/pfrankov/kubik/blob/main/docs/hardware-s3.md") from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--port", help="serial port; auto-selected if exactly one Espressif device is connected")
    parser.add_argument("--check", action="store_true", help="verify the bundle without accessing a device")
    args = parser.parse_args()
    directory = args.bundle.resolve()
    manifest = bundle_files(directory)
    print(f"Bundle profile {manifest['board']} · Kubik {manifest['version']} {manifest['character']}: images verified")
    if args.check:
        return
    port = select_port(args.port)
    flash_images(directory, manifest, port)
    print("Запись завершена. Дождитесь экрана настройки. Если экран пуст, полностью "
          "отключите USB-C; при батарее удерживайте PWR до выключения, затем "
          "удерживайте PWR около 2 с для включения. Подключите USB-C без BOOT.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f"Прошивка не завершена (esptool, код {error.returncode}). "
              "При DOWNLOAD без ответа закройте программы USB-порта и перезапустите питание: "
              "отключите USB-C, при батарее выключите плату долгим нажатием PWR "
              "и включите удержанием PWR около 2 с; подключите USB-C без BOOT и повторите команду.", file=sys.stderr)
        sys.exit(1)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Flash failed: {error}", file=sys.stderr)
        sys.exit(1)
