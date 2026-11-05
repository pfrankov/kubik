#!/usr/bin/env python3
"""Package the firmware and installable OpenClaw channel in one release ZIP."""

import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import tarfile
import tempfile
import zipfile
from firmware_layout import TARGETS, firmware_filename, kit_filename

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--character", choices=("TESS", "PLUSH"), default="TESS")
parser.add_argument("--target", choices=tuple(TARGETS), default="esp32c6")
arguments = parser.parse_args()
character = arguments.character
target = arguments.target
output = root / "dist" / kit_filename(character, target)
firmware_name = firmware_filename(character, target)


def validate_buyer_documents():
    guide = root / "docs/buyer"
    manifest = json.loads((guide / "print-manifest.json").read_text())
    for group, base in (("inputs", root), ("outputs", guide)):
        for name, expected in manifest[group].items():
            if hashlib.sha256((base / name).read_bytes()).hexdigest() != expected:
                raise ValueError("Buyer guide is stale; run tools/build-buyer-guide.py")


def write_member(bundle, name, data, unix_mode=0o100644):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.create_system = 3
    info.external_attr = unix_mode << 16
    info.compress_type = zipfile.ZIP_STORED
    bundle.writestr(info, data, compress_type=zipfile.ZIP_STORED)


def rewrite_acceptance_links(document, revision):
    def rewrite(match):
        target = match.group(1)
        path, marker, fragment = target.partition("#")
        if path.startswith(("scenarios/", "history/", "architecture/")):
            source_path = "docs/" + path
        elif path == "screen-lab.md":
            source_path = "docs/screen-lab.md"
        elif path in {"../firmware/dependencies.lock", "../firmware/dependencies.lock.esp32s3"} or path.startswith("../.github/"):
            source_path = path[3:]
        else:
            return match.group(0)
        anchor = f"#{fragment}" if marker else ""
        return f"](https://github.com/pfrankov/kubik/blob/{revision}/{source_path}{anchor})"

    return re.sub(r"\]\(([^)]+)\)", rewrite, document)


validate_buyer_documents()
revision = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()
if not re.fullmatch(r"[0-9a-f]{40,64}", revision):
    raise ValueError("Cannot determine source revision for acceptance links")
subprocess.run(["python3", str(root / "tools/package-firmware.py"), "--character", character,
                "--target", target], check=True)
with tempfile.TemporaryDirectory(prefix="kubik-plugin-") as temporary:
    result = subprocess.run(
        ["npm", "pack", "--json", "--pack-destination", temporary],
        cwd=root / "openclaw-kubik", check=True, capture_output=True, text=True,
    )
    packages = json.loads(result.stdout)
    if len(packages) != 1 or packages[0]["name"] != "openclaw-kubik":
        raise ValueError("Unexpected plugin package")
    plugin = Path(temporary) / packages[0]["filename"]
    with tarfile.open(plugin) as archive:
        members = {member.name for member in archive.getmembers()}
        if not {"package/package.json", "package/openclaw.plugin.json",
                "package/src/index.js", "package/src/agent-sdk/cli.js",
                "package/src/agent-sdk/index.js", "package/LICENSE", "package/NOTICE"} <= members:
            raise ValueError("Plugin package is incomplete")
    items = [root / "dist" / firmware_name, plugin]
    manifest = {"name": "Kubik kit", "firmware": firmware_name, "character": character.title(),
                "chip": target,
                "board": TARGETS[target]['board'],
                "plugin": plugin.name, "pluginVersion": packages[0]["version"], "parts": {}}
    for item in items:
        manifest["parts"][item.name] = hashlib.sha256(item.read_bytes()).hexdigest()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as bundle:
        for item in items:
            write_member(bundle, item.name, item.read_bytes())
        buyer = root / "docs/buyer"
        content = ((buyer / "START.html").read_text()
                   .replace("../agent-sdk.md", "agent-sdk.md")
                   .replace("../hermes.md", "docs/hermes.md")
                   .replace("../../docs/agent-sdk.md", "agent-sdk.md")
                   .replace("../../deploy/", "deploy/"))
        write_member(bundle, "START.html", content.encode())
        write_member(bundle, "start.js", (buyer / "start.js").read_bytes())
        css = (buyer / "start.css").read_text().replace("../../tools/font/sources/", "assets/")
        write_member(bundle, "start.css", css.encode())
        write_member(bundle, "assets/hello-tess.png", (buyer / "assets/hello-tess.png").read_bytes())
        for style in ("Regular", "Bold"):
            name = f"PT_Sans-Web-{style}.ttf"
            write_member(bundle, f"assets/{name}", (root / "tools/font/sources" / name).read_bytes())
        if character == "TESS":
            write_member(bundle, "Getting-started.pdf", (buyer / "kubik-getting-started-A6.pdf").read_bytes())
        readme = ((root / "docs/KIT.ru.md").read_text().replace("(../deploy/", "(deploy/")
                  .replace("(../README.md)", f"(https://github.com/pfrankov/kubik/blob/{revision}/README.md)")
                  .replace("(native-voice.md)", "(docs/native-voice.md)")
                  .replace("(hardware-s3.md)", "(docs/hardware-s3.md)")
                  .replace("(hardware-s3.md#", "(docs/hardware-s3.md#")
                  .replace("(hermes.md)", "(docs/hermes.md)").replace("(pairing.md)", "(docs/pairing.md)")
                  .replace("(buyer/", "("))
        write_member(bundle, "README.md", readme.encode("utf-8"))
        sdk = re.sub(r"\(protocol\.md(?=[)#])", "(docs/protocol.md",
                     (root / "docs/agent-sdk.md").read_text()).replace("(../openclaw-kubik/", "(openclaw-kubik/").replace("(buyer/", "(").replace("(muse.md)", "(docs/muse.md)").replace("(hermes.md)", "(docs/hermes.md)")
        write_member(bundle, "agent-sdk.md", sdk.encode("utf-8"))
        controls = (root / "docs/agent-controls.md").read_text().replace("(agent-sdk.md)", "(../agent-sdk.md)").replace("(buyer/", "(../")
        write_member(bundle, "docs/agent-controls.md", controls.encode("utf-8"))
        native = (root / "docs/native-voice.md").read_text().replace("(agent-sdk.md)", "(../agent-sdk.md)").replace("(buyer/", "(../").replace("(KIT.ru.md)", "(../README.md)")
        write_member(bundle, "docs/native-voice.md", native.encode("utf-8"))
        for document in ("protocol.md", "acceptance.md", "muse.md", "hermes.md", "events.md", "pairing.md", "hardware-s3.md"):
            content = (root / "docs" / document).read_text()
            if document == "acceptance.md":
                content = rewrite_acceptance_links(content, revision)
            if document == "hardware-s3.md":
                target = f"https://github.com/pfrankov/kubik/blob/{revision}/docs/scenarios/tess-skies-entry.md"
                content = content.replace("(scenarios/tess-skies-entry.md)", f"({target})")
            content = content.replace("(agent-sdk.md)", "(../agent-sdk.md)").replace("(buyer/", "(../").replace("(KIT.ru.md)", "(../README.md)")
            write_member(bundle, f"docs/{document}", content.encode("utf-8"))
        write_member(bundle, "docs/scenarios/hermes.md", (root / "docs/scenarios/hermes.md").read_bytes())
        write_member(bundle, "tools/test-hermes-native.py",
                     (root / "tools/test-hermes-native.py").read_bytes())
        for path in sorted((root / "tools/hermes_native").iterdir()):
            if path.is_file() and path.suffix in (".py", ".json"):
                write_member(bundle, path.relative_to(root).as_posix(), path.read_bytes())
        for path in sorted((root / "hermes-kubik").iterdir()):
            if path.is_file() and path.suffix in (".py", ".yaml", ".txt"):
                write_member(bundle, path.relative_to(root).as_posix(), path.read_bytes())
        write_member(bundle, "tools/install-hermes.py", (root / "tools/install-hermes.py").read_bytes())
        for notice in (root / "LICENSE", root / "NOTICE", *sorted((root / "licenses").iterdir())):
            write_member(bundle, notice.relative_to(root).as_posix(), notice.read_bytes())
        write_member(bundle, "openclaw-kubik/README.md",
                     (root / "openclaw-kubik/README.md").read_bytes())
        write_member(bundle, "deploy/cloudflared/README.md",
                     (root / "deploy/cloudflared/README.md").read_bytes())
        write_member(bundle, "deploy/cloudflared/probe.mjs",
                     (root / "deploy/cloudflared/probe.mjs").read_bytes())
        write_member(bundle, "deploy/cloudflared/install-systemd.sh",
                     (root / "deploy/cloudflared/install-systemd.sh").read_bytes(),
                     unix_mode=0o100755)
        write_member(bundle, "manifest.json",
                     (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
print(f"{output}: plugin {packages[0]['version']}, {output.stat().st_size:,} bytes")
