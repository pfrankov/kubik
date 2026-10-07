#!/usr/bin/env python3
"""Check the installable release kit and its reproducible packaging."""

import argparse
from hashlib import sha256
from io import BytesIO
import json
import posixpath
from pathlib import PurePosixPath, Path
import re
import subprocess
import sys
import tarfile
import zipfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--character", choices=("TESS", "PLUSH"), default="TESS")
character = parser.parse_args().character
firmware_name = f"kubik-firmware-{character.lower()}.zip"
kit_path = Path(__file__).resolve().parent.parent / f"dist/kubik-kit-{character.lower()}.zip"
source_root = kit_path.parent.parent
version_header = (source_root / "firmware/main/version.h").read_text()
firmware_version = re.search(r'#define KUBIK_FW_VERSION "([^"]+)"', version_header).group(1)
plugin_version = json.loads((source_root / "openclaw-kubik/package.json").read_text())["version"]


def check_cloudflared_source():
    guide = (source_root / "deploy/cloudflared/README.md").read_text()
    installer_path = "deploy/cloudflared/install-systemd.sh"
    installer = (source_root / installer_path).read_text()
    assert "`cloudflared` 2025.4.0 или новее" in guide
    assert "Node.js 24.16 или новее (24.x) либо 26.1 или новее" in guide
    assert not re.search(r"\b(or later|only|or >=)\b", guide.replace("`", "")), "English words in the Russian guide"
    assert "http://127.0.0.1:18789" in guide  # the OpenClaw Gateway port: the plugin serves /kubik/v1 there
    assert "Path: `/kubik/v1` (только этот путь" in guide
    for feature in ("read -r -s tunnel_token", "--token-file $token_file",
                    "chmod 0600", "User=$service_user", "Restart=on-failure",
                    "RestartSec=10s", "StartLimitIntervalSec=0"):
        assert feature in installer, feature
    assert "tunnel run --token " not in installer
    assert "StartLimitBurst=" not in installer
    for cleanup_action in ("trap cleanup EXIT", "if (( completed == 0 )); then",
                           'systemctl disable --now "$unit_name"',
                           'rm -f -- "$unit_file"', 'rm -f -- "$token_file"',
                           'rmdir -- "$state_dir"', 'userdel "$service_user"'):
        assert cleanup_action in installer, cleanup_action
    print("cloudflared source static integration: passed", flush=True)


check_cloudflared_source()


def package_hashes():
    subprocess.run([sys.executable, str(source_root / "tools/package-kit.py"), "--character", character], check=True)
    data = kit_path.read_bytes()
    with zipfile.ZipFile(BytesIO(data)) as package:
        manifest = json.loads(package.read("manifest.json"))
        return (sha256(data).hexdigest(),
                sha256(package.read(firmware_name)).hexdigest(),
                sha256(package.read(manifest["plugin"])).hexdigest())


def check_acceptance_links(contents):
    revision = subprocess.run(["git", "-C", str(source_root), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    source = (source_root / "docs/acceptance.md").read_text()
    source_targets = set(re.findall(r"\]\(([^)]+)\)", source))
    tracked = set(subprocess.run(["git", "-C", str(source_root), "ls-tree", "-r", "--name-only", revision],
                                 check=True, capture_output=True, text=True).stdout.splitlines())
    blob_prefix = "https://github.com/pfrankov/kubik/blob/"
    linked = set()

    def restore_link(match):
        target = match.group(1)
        if target in source_targets or not target.startswith(blob_prefix): return match.group(0)
        pinned_revision, separator, path_and_fragment = target[len(blob_prefix):].partition("/")
        assert separator and pinned_revision == revision, target
        path, marker, fragment = path_and_fragment.partition("#")
        assert path in tracked and (source_root / path).is_file(), target
        linked.add(path)
        anchor = f"#{fragment}" if marker else ""
        return f"]({posixpath.relpath(path, 'docs')}{anchor})"

    restored = re.sub(r"\]\(([^)]+)\)", restore_link, contents)
    assert "docs/architecture/connection-storage.md" in linked

    if "agent-sdk.md" in source_targets:
        restored = restored.replace("(../agent-sdk.md)", "(agent-sdk.md)")
    if "KIT.ru.md" in source_targets:
        restored = restored.replace("(../README.md)", "(KIT.ru.md)")
    for target in source_targets:
        if target.startswith("buyer/"):
            restored = restored.replace(f"](../{target[6:]})", f"]({target})")
    assert restored == source, "packaged acceptance content or source links changed"


first_hashes = package_hashes()
second_hashes = package_hashes()
assert first_hashes == second_hashes, ("release packaging changed without source changes: "
                                        f"{first_hashes} != {second_hashes}")

with zipfile.ZipFile(kit_path) as kit:
    names = set(kit.namelist())
    required = {"START.html", "start.css", "start.js", "assets/hello-tess.png", "assets/PT_Sans-Web-Regular.ttf", "assets/PT_Sans-Web-Bold.ttf",
                "README.md", "manifest.json", firmware_name, "LICENSE", "NOTICE", "licenses/README.txt",
                "openclaw-kubik/README.md", "deploy/cloudflared/README.md",
                "deploy/cloudflared/probe.mjs", "deploy/cloudflared/install-systemd.sh",
                "hermes-kubik/adapter.py", "hermes-kubik/plugin.yaml", "hermes-kubik/pair.py",
                "tools/install-hermes.py", "docs/hermes.md", "docs/events.md"}
    assert required <= names, sorted(required - names)
    assert not {"VOICE.html", "CONNECT.html", "ASK-AGENT.txt"} & names, 'Obsolete split buyer flow'
    contents = kit.read("START.html").decode()
    assert 'lang="en"' in contents and not re.search('[А-Яа-яЁё]', contents)
    for target in re.findall(r'(?:href|src)="([^"]+)"', contents):
        if target.startswith(("https:", "http:", "#")): continue
        assert target.split('#')[0] in names, ("START.html", target)
    if character == "TESS":
        assert kit.read("Getting-started.pdf") == (source_root / "docs/buyer/kubik-getting-started-A6.pdf").read_bytes()
    manifest = json.loads(kit.read("manifest.json"))
    assert manifest["pluginVersion"] == plugin_version
    assert set(manifest["parts"]) == {manifest["firmware"], manifest["plugin"]}
    for name, digest in manifest["parts"].items():
        assert name in names and sha256(kit.read(name)).hexdigest() == digest

    check_acceptance_links(kit.read("docs/acceptance.md").decode())

    for name in sorted(name for name in names if PurePosixPath(name).suffix.lower() in {".md", ".markdown"}):
        contents = kit.read(name).decode()
        for target in re.findall(r"\]\(([^)]+)\)", contents):
            if target.startswith(("https:", "http:", "#")):
                continue
            relative = target.split("#", 1)[0]
            resolved = PurePosixPath(name).parent.joinpath(relative)
            parts = []
            for part in resolved.parts:
                if part == "..":
                    assert parts
                    parts.pop()
                elif part not in ("", "."):
                    parts.append(part)
            assert "/".join(parts) in names, (name, target)

    manual = kit.read("README.md").decode()
    archive = f"openclaw-kubik-{plugin_version}.tgz"
    assert f"openclaw plugins install ./{archive} " in manual and f"openclaw plugins install .\\{archive} " in manual
    assert "npm-pack:" not in manual and "/tmp/kubik-plugin.tgz" not in manual
    for promise in ("уже прошитым", "только по USB", "Доверие при первом подключении", "commands.ownerAllowFrom",
                    "Экспериментально", "не поддерживается", "нельзя достучаться снаружи"):
        assert promise in manual, promise
    installer_path = "deploy/cloudflared/install-systemd.sh"
    installer_info = kit.getinfo(installer_path)
    assert installer_info.external_attr >> 16 & 0o777 == 0o755
    installer = kit.read(installer_path).decode()
    assert installer == (source_root / installer_path).read_text()
    for product_notice in ("LICENSE", "NOTICE"):
        assert kit.read(product_notice) == (source_root / product_notice).read_bytes()
    assert b"Apache License" in kit.read("LICENSE") and b"Pavel Frankov" in kit.read("NOTICE")
    for notice in (source_root / "licenses").iterdir():
        assert kit.read(f"licenses/{notice.name}") == notice.read_bytes(), notice.name
    index = kit.read("licenses/README.txt").decode()
    for component in ("ESP-IDF", "Mbed TLS", "esp_codec_dev", "esp_websocket_client", "esp_lcd_touch",
                      "esp_lcd_touch_cst9217", "qrcode", "FreeRTOS", "lwIP", "PT Sans", "`ws`"):
        assert component in index, component
    for name in re.findall(r"^  ((?:Apache|BSD|MIT|OFL|http)[\w.*-]*)", index, re.M):
        assert any(PurePosixPath(n).match(f"licenses/{name}") for n in names), name
    assert kit.read("deploy/cloudflared/README.md").decode() == (source_root / "deploy/cloudflared/README.md").read_text()

    with zipfile.ZipFile(BytesIO(kit.read(manifest["firmware"]))) as firmware:
        firmware_manifest = json.loads(firmware.read("manifest.json"))
        assert firmware_manifest["version"] == firmware_version
        assert firmware_manifest["character"] == manifest["character"] == character.title()
        notices = {file.name: file.read_bytes() for file in (source_root / "licenses").iterdir() if file.is_file()}
        assert {"flash-device.py", "INSTALL.ru.txt", "LICENSE", "NOTICE"} | {f"licenses/{name}" for name in notices} <= set(firmware.namelist())
        for name, content in notices.items():
            assert firmware.read(f"licenses/{name}") == content
        assert b"Copyright (c) Project Nayuki" in firmware.read("licenses/MIT-Nayuki-qrcodegen.txt")
        assert b"Dave Gamble and cJSON contributors" in firmware.read("licenses/MIT-cJSON.txt")
        assert b"Jouni Malinen" in firmware.read("licenses/BSD-wpa-supplicant.txt")
        assert b"Cozybit, Inc." in firmware.read("licenses/BSD-wpa-supplicant.txt")
        assert b"The Linux Foundation" in firmware.read("licenses/BSD-wpa-supplicant.txt")
        assert b"Amazon.com, Inc." in firmware.read("licenses/MIT-FreeRTOS.md")
        assert b"Axon Digital Design" in firmware.read("licenses/BSD-lwIP-Leon-dhcp.txt")
        assert b"The MINIX 3 Project" in firmware.read("licenses/BSD-MINIX-tcp-isn.txt")
        for prefix, expected_hash in (("Newlib-COPYING", "422aa40293093fb54fc66e692a0d68fd0b24ed5602e5d1d33ad05ba3909057e9"),
                                      ("GCC-COPYING3", "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903")):
            parts = sorted(name for name in notices if name.startswith(prefix + "-"))
            assert parts and sha256(b"".join(firmware.read(f"licenses/{name}") for name in parts)).hexdigest() == expected_hash
        for part in firmware_manifest["parts"]:
            assert sha256(firmware.read(part["file"])).hexdigest() == part["sha256"]

    with tarfile.open(fileobj=BytesIO(kit.read(manifest["plugin"]))) as plugin:
        contents = {member.name for member in plugin.getmembers()}
        assert {"package/LICENSE", "package/NOTICE", "package/package.json", "package/openclaw.plugin.json"} <= contents
        assert plugin.extractfile("package/LICENSE").read() == (source_root / "LICENSE").read_bytes()
        assert plugin.extractfile("package/NOTICE").read() == (source_root / "NOTICE").read_bytes()
        package = json.load(plugin.extractfile("package/package.json"))
        assert package["version"] == manifest["pluginVersion"]
        assert package["exports"]["./agent-sdk"] == "./src/agent-sdk/index.js"
        assert package["bin"]["kubik-agent-host"] == "./src/agent-sdk/cli.js"
        assert {"package/src/agent-sdk/index.js", "package/src/agent-sdk/cli.js"} <= contents
        # Reverse relocation to check all original content, including anchored links.
        sdk = kit.read("agent-sdk.md").decode()
        restored = sdk.replace("(docs/protocol.md", "(protocol.md").replace("(openclaw-kubik/", "(../openclaw-kubik/")
        restored = restored.replace("(START.html", "(buyer/START.html").replace("(docs/muse.md)", "(muse.md)").replace("(docs/hermes.md)", "(hermes.md)")
        assert restored == (source_root / "docs/agent-sdk.md").read_text()
        assert "(docs/protocol.md#agent-controls-and-speech-capabilities-062)" in sdk
        assert "(protocol.md#" not in sdk
print("kit: versions, hashes, licenses, and installer documentation passed")
