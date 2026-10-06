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


def bundle_files(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("chip") != "esp32c6" or manifest.get("flash_size") != "16MB":
        raise ValueError("This bundle is not for the 16 MB Kubik ESP32-C6")
    if manifest.get("character") not in {"Tess", "Plush"}:
        raise ValueError("Firmware character is missing or invalid")
    parts = manifest.get("parts")
    if not isinstance(parts, list) or len(parts) != 4:
        raise ValueError("Expected bootloader, partition table, app and assets")
    assets_offset = 0x320000 if manifest['character'] == 'Tess' else 0x200000
    expected_offsets = {0, 0x8000, 0x20000, assets_offset}
    if {part.get("offset") for part in parts} != expected_offsets:
        raise ValueError("Unexpected flash layout")
    ordered = sorted(parts, key=lambda part: part["offset"])
    validate_images(directory, ordered)
    return manifest


def validate_images(directory, ordered):
    for index, part in enumerate(ordered):
        name = part.get("file")
        if not isinstance(name, str) or Path(name).name != name:
            raise ValueError("Invalid image name")
        data = (directory / name).read_bytes()
        if len(data) != part.get("size") or hashlib.sha256(data).hexdigest() != part.get("sha256"):
            raise ValueError(f"Image failed integrity check: {name}")
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
        raise RuntimeError("Install esptool first: python3 -m pip install esptool==4.12.0") from error
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--port", help="serial port; auto-selected if exactly one Espressif device is connected")
    parser.add_argument("--check", action="store_true", help="verify the bundle without accessing a device")
    args = parser.parse_args()
    directory = args.bundle.resolve()
    manifest = bundle_files(directory)
    print(f"Kubik {manifest['version']} · {manifest['character']}: all four images verified")
    if args.check:
        return
    port = select_port(args.port)
    command = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "--port", port,
               "--baud", "460800", "--before", "default_reset", "--after", "hard_reset",
               "write_flash", "--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"]
    for part in sorted(manifest["parts"], key=lambda part: part["offset"]):
        command.extend([hex(part["offset"]), str(directory / part["file"])])
    stop_bridges(port)
    subprocess.run(command, check=True)
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
