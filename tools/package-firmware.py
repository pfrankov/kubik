#!/usr/bin/env python3
"""Create a verified, self-contained flash bundle from an ESP-IDF build."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import zipfile
from firmware_layout import partitions, partition_table


ROOT = Path(__file__).resolve().parent.parent
def images(character):
    table = partitions(character)
    return ((0, "bootloader.bin", "bootloader/bootloader.bin"),
            (0x8000, "partition-table.bin", "partition_table/partition-table.bin"),
            (table['factory']['offset'], "kubik.bin", "kubik.bin"),
            (table['assets']['offset'], "assets.bin", "assets.bin"))


MIN_APP_FREE = 0.02  # of the app partition: room for the next features and for compiler differences between machines


def app_partition_size(character='TESS'):
    return partitions(character)['factory']['size']


def check_app_margin(size, partition):
    free = partition - size
    if free < partition * MIN_APP_FREE:
        raise ValueError(f"kubik.bin is {size:,} of {partition:,} bytes: {free:,} ({free / partition:.1%}) free, "
                         f"at least {MIN_APP_FREE:.0%} must stay free; shrink the firmware before releasing it")
    return free


def write_member(bundle, name, data):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    bundle.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)


def check_character(build, character):
    if (build / "character.txt").read_text().strip() != character:
        raise ValueError("Build character differs from the requested package; use its own build directory")


def build_inputs(build):
    # ESP-IDF's selected sources and their compiler dependencies are authoritative.
    description = json.loads((build / "project_description.json").read_text())
    selected = {Path(path).resolve() for path in description["build_component_info"]["main"]["sources"]}
    inputs = set()
    for entry in json.loads((build / "compile_commands.json").read_text()):
        source = Path(entry["file"]).resolve()
        if source not in selected or not source.is_relative_to(ROOT / "firmware/main"): continue
        dependency = build / (entry["output"] + ".d")
        if not dependency.is_file(): raise ValueError("Missing compiler dependencies; rebuild firmware")
        body = dependency.read_text().split(":", 1)[1].replace("\\\n", " ")
        for name in shlex.split(body):
            path = Path(name)
            if not path.is_absolute(): path = Path(entry["directory"]) / path
            path = path.resolve()
            if path.is_relative_to(ROOT / "firmware/main"): inputs.add(path)
    if not inputs: raise ValueError("Missing main source dependencies; rebuild firmware")
    inputs.update(path for path in (ROOT / "firmware/main").iterdir() if path.suffix in {".html", ".css", ".js"})
    character = (build / 'character.txt').read_text().strip()
    inputs.update([ROOT / "firmware/main/CMakeLists.txt", ROOT / "firmware/CMakeLists.txt",
                   partition_table(character), Path(description['config_file']),
                   ROOT / "firmware/sdkconfig.defaults", ROOT / "tools/font/gen-font.swift",
                   ROOT / "tools/font/write-font.py", *(ROOT / "tools/font/sources").glob("*.ttf")])
    return inputs


def check_build_freshness(build):
    if max(path.stat().st_mtime for path in build_inputs(build)) > (build / "kubik.bin").stat().st_mtime:
        raise ValueError("Firmware sources changed after the build; run idf.py -C firmware build")


def package(build, output, character):
    check_character(build, character)
    check_build_freshness(build)
    with tempfile.TemporaryDirectory(prefix="kubik-assets-") as directory:
        regenerated = Path(directory) / "assets.bin"
        subprocess.run([sys.executable, str(ROOT / "tools/assets/pack.py"), str(regenerated), "--character", character],
                       check=True, capture_output=True)
        if hashlib.sha256(regenerated.read_bytes()).digest() != hashlib.sha256(
                (build / "assets.bin").read_bytes()).digest():
            raise ValueError("Built assets differ from the current source assets; rebuild firmware")
    free = check_app_margin((build / "kubik.bin").stat().st_size, app_partition_size(character))
    version_header = (ROOT / "firmware/main/version.h").read_text()
    version = re.search(r'#define KUBIK_FW_VERSION "([^"]+)"', version_header)
    if not version:
        raise ValueError("Firmware version is missing")
    flashed = json.loads((build / "flasher_args.json").read_text())["flash_files"]
    expected = {hex(offset): relative for offset, _, relative in images(character)}
    if flashed != expected:
        raise ValueError("Build flash layout differs from the Kubik release layout")
    manifest = {"name": "Kubik", "version": version.group(1), "chip": "esp32c6",
                "flash_size": "16MB", "character": character.title(), "parts": []}
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6) as bundle:
        for offset, name, relative in images(character):
            data = (build / relative).read_bytes()
            if offset + len(data) > 0x1000000:
                raise ValueError(f"{name} exceeds the 16 MB flash")
            manifest["parts"].append({"offset": offset, "file": name, "size": len(data),
                                      "sha256": hashlib.sha256(data).hexdigest()})
            write_member(bundle, name, data)
        write_member(bundle, "manifest.json",
                     (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        write_member(bundle, "flash-device.py", (ROOT / "tools/flash-device.py").read_bytes())
        write_member(bundle, "INSTALL.ru.txt", (ROOT / "docs/INSTALL.ru.txt").read_bytes())
        for notice in (ROOT / "LICENSE", ROOT / "NOTICE", *sorted((ROOT / "licenses").iterdir())):
            write_member(bundle, notice.relative_to(ROOT).as_posix(), notice.read_bytes())
    print(f"{output}: Kubik {manifest['version']}, {output.stat().st_size:,} bytes, app partition {free:,} bytes free")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--character", choices=("TESS", "PLUSH"), default="TESS")
    arguments = parser.parse_args()
    if arguments.build is None:
        arguments.build = ROOT / ("firmware/build" if arguments.character == "TESS" else "firmware/build-plush")
    if arguments.output is None:
        arguments.output = ROOT / f"dist/kubik-firmware-{arguments.character.lower()}.zip"
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        package(arguments.build, arguments.output, arguments.character)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Cannot package firmware: {error}", file=sys.stderr)
        sys.exit(1)
