#!/usr/bin/env python3
"""Exercise real ESP-IDF 5.5.1 NVS recovery on a 24 KiB emulated NOR partition."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
idf = Path(os.environ.get("IDF_PATH", ""))
if not (idf / "components/nvs_flash/src/nvs_storage.cpp").is_file():
    raise SystemExit("Activate ESP-IDF 5.5.1 before running tools/test-nvs.py")
partitions = (ROOT / "firmware/partitions.csv").read_text().splitlines()
nvs = next(line.split(",") for line in partitions if line.split(",")[0].strip() == "nvs")
if int(nvs[4].strip(), 0) != 24 * 1024:
    raise SystemExit("Update the NVS fault fixture to cover the changed partition size")

with tempfile.TemporaryDirectory(prefix="kubik-nvs-") as directory:
    temp = Path(directory)
    (temp / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_IDF_TARGET_LINUX 1\n")
    # Only platform logging is stubbed. Storage, pages, CRC and all NVS error constants are IDF's own code.
    (temp / "esp_log.h").write_text("#pragma once\n" + "".join(
        f"#define ESP_LOG{level}(...) ((void)0)\n" for level in "DEWIV"))
    includes = [temp, ROOT / "firmware/main", ROOT / "firmware/sim"]
    includes += [idf / "components" / path for path in (
        "nvs_flash/src", "nvs_flash/include", "nvs_flash/private_include", "esp_common/include",
        "esp_rom/include", "esp_rom/linux", "esp_partition/include", "spi_flash/include", "heap/include")]
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-DNO_DEBUG_STORAGE", "-DLINUX_TARGET=1"]
    flags += ["-I" + str(path) for path in includes]
    crc = temp / "crc.o"
    subprocess.run([os.environ.get("CC", "cc"), *flags, "-c",
                    str(idf / "components/esp_rom/linux/esp_rom_crc.c"), "-o", str(crc)], check=True)
    sources = [idf / f"components/nvs_flash/src/{name}.cpp" for name in (
        "nvs_storage", "nvs_page", "nvs_pagemanager", "nvs_item_hash_list", "nvs_types")]
    exe = temp / "nvs-test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", *flags,
                    str(ROOT / "firmware/sim/nvs_flash_test.cpp"), *map(str, sources), str(crc),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
