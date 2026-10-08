#!/usr/bin/env python3
"""Single acceptance entry point. Default: lint, host, fresh firmware and clean kit install."""
import argparse
import ipaddress
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
PLAYWRIGHT = "playwright==1.63.0"
IDF_TOOLS = {"host": ("idf.py", "clang"), "host-portable": ("idf.py", "clang"), "build": ("idf.py",), "ci": ("idf.py", "clang")}
HOST_TESTS = (
    "agent-menu", "tls-memory", "audio-capture", "mic-task", "mic-delivery", "audio-stream", "audio-levels", "menu-hold", "audio-wake", "wake-word", "native-voice", "guide", "ima", "link", "navigation", "pin", "power-network",
    "event-journal", "render", "frames", "screen-lab", "lab-isolation", "ws-write", "settings", "nvs", "setup", "muse", "devkey", "hermes", "speech", "state", "tess", "tess-games", "wifi", "radio-relay",
)


def run(command, cwd=ROOT, timeout=600):
    print("\nRUN " + " ".join(map(str, command)), flush=True)
    subprocess.run(command, cwd=cwd, check=True, timeout=timeout)


def validate_device_environment():
    if os.environ.get("KUBIK_TEST_LIVE") == "1": raise RuntimeError("Acceptance requires local mocks, not paid live calls")
    try: ipaddress.IPv4Address(os.environ.get("KUBIK_TEST_LAN_HOST", ""))
    except ipaddress.AddressValueError as error: raise RuntimeError("Set KUBIK_TEST_LAN_HOST to a reachable computer IPv4") from error
    relay = os.environ.get("KUBIK_TEST_RELAY_SSH")
    relay_ip = os.environ.get("KUBIK_TEST_RELAY_IPV4")
    if relay or relay_ip:
        if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9@._:-]*", relay or "") or not shutil.which("ssh"):
            raise RuntimeError("Relay requires a valid explicit SSH target and ssh")
        try: ipaddress.IPv4Address(relay_ip or "")
        except ipaddress.AddressValueError as error: raise RuntimeError("Set KUBIK_TEST_RELAY_IPV4 for the relay") from error


def require_device_voice():
    voices = subprocess.check_output(["say", "-v", "?"], text=True)
    if not any(line.startswith("Milena ") and "ru_RU" in line for line in voices.splitlines()):
        raise RuntimeError("Install the Russian Milena voice before device acceptance")


def preflight(level):
    required = ["node", "npm", "uv", "uvx", "cc", "git", "tar", "unzip", "openssl"]
    required.extend(IDF_TOOLS.get(level, ()))
    if level in IDF_TOOLS: required.append("ninja")
    if level == "device":
        required += ["say"]
        validate_device_environment()
    missing = [tool for tool in required if not shutil.which(tool)]
    if missing: raise RuntimeError("Missing required tools: " + ", ".join(missing))
    if level == "device": require_device_voice()
    if level in {"host", "host-portable", "ci"} and hasattr(os, "geteuid") and os.geteuid() == 0:
        raise RuntimeError("Run acceptance as a regular user: root skips the private-key permission test")
    if level in {"host", "host-portable", "build", "ci"}:
        version = subprocess.check_output(["idf.py", "--version"], text=True)
        if "v5.5.1" not in version: raise RuntimeError("Activate ESP-IDF v5.5.1 before acceptance")
        # Fetches the QR component used by native host harnesses on a clean checkout.
        run(["idf.py", "-C", "firmware", "-DKUBIK_CHARACTER=TESS", "reconfigure"])
    if level != "lint": run(["npm", "ci"], ROOT / "tools")


def lint():
    run([sys.executable, "tools/test-lint.py"])
    run(["uv", "run", "--with", "pyyaml==6.0.3", "python", "tools/lint-repo.py"])
    run(["uvx", "--from", "lizard==1.24.0", "python", "tools/check-shape.py"])


def host(portable=False):
    plugin = ROOT / "openclaw-kubik"
    run(["npm", "ci"], plugin)
    run(["node", "tools/check-live-voice.mjs"])
    for script in ("check", "check:host", "check:install"):
        run(["npm", "run", script], plugin)
    bridge = ROOT / "tools/usb-bridge"
    run(["npm", "ci"], bridge)
    run(["node", "--test", *[str(p) for p in sorted(bridge.glob("*.test.mjs"))]])
    run(["node", "--test", "tools/test-device/frames.test.mjs", "tools/test-device/mock.test.mjs", "tools/test-device/diagnostics.test.mjs", "tools/test-device/acoustic.test.mjs", "tools/test-install/device.test.mjs"])
    run(["uv", "run", "--with", "numpy==2.2.6", "python", "tools/test-device/analyze-live-acoustic.test.py"])
    run(["node", "tools/test-portal.mjs"])
    for name in HOST_TESTS:
        if portable and name == "frames":
            run([sys.executable, "tools/test-frames.py", "--structure-only"])
            continue
        run([sys.executable, f"tools/test-{name}.py"])
    run(["uv", "run", "--with", PLAYWRIGHT, "python", "tools/test-portal-browser.py"])
    run(["uv", "run", "--with", PLAYWRIGHT, "python", "tools/test-emulator-browser.py"])
    run(["uv", "run", "--with", PLAYWRIGHT, "--with", "pymupdf==1.26.7", "python", "tools/test-buyer-guide.py"])


def build():
    for character, directory in (("TESS", "build"), ("PLUSH", "build-plush")):
        run(["idf.py", "-C", "firmware", "-B", str(ROOT / "firmware" / directory), f"-DKUBIK_CHARACTER={character}", "build"])
        run([sys.executable, "tools/test-package.py", "--character", character])
        run([sys.executable, "tools/test-kit.py", "--character", character])
    run(["node", "tools/test-install.mjs", "--kit", "dist/kubik-kit-tess.zip", "--openclaw", "2026.9.7"])


def device():
    run(["npm", "ci"], ROOT / "openclaw-kubik")
    run(["npm", "ci"], ROOT / "tools/usb-bridge")
    # Guide preflight runs before recording tests; an unfinished first guide requires explicit provisioning.
    run(["node", "tools/test-device-guide.mjs", "--reboot", "--profile"])
    # The bridge must already target the local mock; these tools restore identity and settings.
    run(["node", "tools/test-device-navigation.mjs"])
    run(["node", "tools/test-device-screen-lab.mjs"])
    run(["node", "tools/test-device-events.mjs"])
    run(["node", "tools/test-device-sound.mjs"])
    run(["node", "tools/test-device-idle.mjs"])
    run(["node", "tools/test-device-cpu.mjs"])
    run(["node", "tools/test-device-native-voice.mjs", "--barge"])
    for profile in (["--single-delta"], ["--paced", "--small-deltas"]):
        run(["node", "tools/test-device-native-voice.mjs", "--wifi", "--speech-only", *profile])
    run(["node", "tools/test-device-capture.mjs"])
    run(["node", "tools/test-device-agent.mjs", "--select"])
    run(["node", "tools/test-device-notify.mjs"])
    run(["node", "tools/test-device-radio.mjs"])
    run(["node", "tools/test-device.mjs"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("level", nargs="?", choices=("lint", "host", "host-portable", "build", "device", "ci"), default="ci")
    args = parser.parse_args()
    started = time.monotonic()
    try:
        preflight(args.level)
        if args.level == "build": run(["npm", "ci"], ROOT / "openclaw-kubik")
        if args.level in {"lint", "ci"}: lint()
        if args.level in {"host", "host-portable", "ci"}: host(portable=args.level == "host-portable")
        if args.level in {"build", "ci"}: build()
        if args.level == "device": device()
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(f"ACCEPTANCE FAILED: {error}", file=sys.stderr)
        return 1
    print(f"ACCEPTANCE PASSED: {args.level} ({time.monotonic() - started:.0f} s)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
