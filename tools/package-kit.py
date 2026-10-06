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

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--character", choices=("TESS", "PLUSH"), default="TESS")
character = parser.parse_args().character
output = root / f"dist/kubik-kit-{character.lower()}.zip"
firmware_name = f"kubik-firmware-{character.lower()}.zip"


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


validate_buyer_documents()
subprocess.run(["python3", str(root / "tools/package-firmware.py"), "--character", character], check=True)
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
                "plugin": plugin.name, "pluginVersion": packages[0]["version"], "parts": {}}
    for item in items:
        manifest["parts"][item.name] = hashlib.sha256(item.read_bytes()).hexdigest()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as bundle:
        for item in items:
            write_member(bundle, item.name, item.read_bytes())
        buyer = root / "docs/buyer"
        content = (buyer / "START.html").read_text().replace("../agent-sdk.md", "agent-sdk.md").replace("../../docs/agent-sdk.md", "agent-sdk.md").replace("../../deploy/", "deploy/")
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
        write_member(bundle, "README.md", (root / "docs/KIT.ru.md").read_text().replace("(../deploy/", "(deploy/").replace("(native-voice.md)", "(docs/native-voice.md)").replace("(hermes.md)", "(docs/hermes.md)").replace("(buyer/", "(").encode("utf-8"))
        sdk = re.sub(r"\(protocol\.md(?=[)#])", "(docs/protocol.md",
                     (root / "docs/agent-sdk.md").read_text()).replace("(../openclaw-kubik/", "(openclaw-kubik/").replace("(buyer/", "(").replace("(muse.md)", "(docs/muse.md)").replace("(hermes.md)", "(docs/hermes.md)")
        write_member(bundle, "agent-sdk.md", sdk.encode("utf-8"))
        controls = (root / "docs/agent-controls.md").read_text().replace("(agent-sdk.md)", "(../agent-sdk.md)").replace("(buyer/", "(../")
        write_member(bundle, "docs/agent-controls.md", controls.encode("utf-8"))
        native = (root / "docs/native-voice.md").read_text().replace("(agent-sdk.md)", "(../agent-sdk.md)").replace("(buyer/", "(../").replace("(KIT.ru.md)", "(../README.md)")
        write_member(bundle, "docs/native-voice.md", native.encode("utf-8"))
        for document in ("protocol.md", "acceptance.md", "muse.md", "hermes.md", "events.md"):
            content = (root / "docs" / document).read_text().replace("(agent-sdk.md)", "(../agent-sdk.md)").replace("(buyer/", "(../").replace("(KIT.ru.md)", "(../README.md)")
            write_member(bundle, f"docs/{document}", content.encode("utf-8"))
        for path in sorted((root / "hermes-kubik").iterdir()):
            if path.is_file() and path.suffix in (".py", ".yaml"):
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
