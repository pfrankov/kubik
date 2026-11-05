#!/usr/bin/env python3
"""Interactive native UI emulator. No USB, agent service or provider keys required."""
import argparse
import atexit
import json
import os
from pathlib import Path
import runpy
import re
import select
import signal
import struct
import subprocess
from socketserver import TCPServer
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "tools/emulator"
TARGETS = ("esp32c6", "esp32s3")


def build(target="esp32c6"):
    if target not in TARGETS: raise ValueError("Invalid firmware target")
    for dependency in [ROOT / "firmware/assets/sprites.bin", ROOT / "firmware/managed_components/espressif__qrcode/qrcodegen.c"]:
        if not dependency.is_file(): raise RuntimeError(f"Missing {dependency.relative_to(ROOT)}; prepare development assets and ESP-IDF dependencies first")
    sources = runpy.run_path(str(ROOT / "tools/test-frames.py"))["SOURCES"]
    sources += ["main/screen_lab", "main/screen_lab_draw", "main/screen_lab_events", "main/screen_lab_fixture", "sim/sim_interactive"]
    output = ROOT / "dist/emulator" / ("kubik-ui" if target == "esp32c6" else "kubik-ui-esp32s3")
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-DKUBIK_SIM_EMBEDDED", f"-DCONFIG_IDF_TARGET_{target.upper()}=1", "-I" + str(ROOT / "firmware/main"),
                    "-I" + str(ROOT / "firmware/managed_components/espressif__qrcode"),
                    *[str(ROOT / f"firmware/{source}.c") for source in sources], "-lm", "-o", str(output)], check=True)
    return output


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(rgb, width, height):
    if len(rgb) != width * height * 3: raise ValueError("Invalid native RGB dimensions")
    stride = width * 3
    rows = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 1)) + chunk(b"IEND", b"")


class Worker:
    def __init__(self, executable):
        self.lock = threading.Lock()
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0,
                                        env={**os.environ, "KUBIK_SPRITES": str(ROOT / "firmware/assets/sprites.bin")})
        try:
            self.send("metadata")
            self.metadata = self.read_json_line()
            self.validate_metadata()
            self.width, self.height = self.metadata["width"], self.metadata["height"]
            self.frame_bytes = self.width * self.height * 3
            self.send("catalog")
            self.screens = self.read_json_line()
            if not isinstance(self.screens, list) or not all(isinstance(name, str) for name in self.screens):
                raise RuntimeError("Invalid native catalog")
        except Exception:
            self.close(); raise
        self.last_frame = 0
        self.touch_deadline = 0
        atexit.register(self.close)

    def read_json_line(self):
        data = bytearray()
        deadline = time.monotonic() + 5
        fd = self.process.stdout.fileno()
        while len(data) < 2048:
            if not select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
                raise RuntimeError("Native metadata timed out")
            part = os.read(fd, 1)
            if not part: raise RuntimeError("Native renderer ended during startup")
            if part == b"\n": return json.loads(data)
            data.extend(part)
        raise RuntimeError("Invalid native metadata header")

    def validate_metadata(self):
        data = self.metadata
        if not isinstance(data, dict) or data.get("target") not in TARGETS:
            raise RuntimeError("Invalid native target metadata")
        if any(type(data.get(key)) is not int or not 1 <= data[key] <= 1024 for key in ("width", "height", "render_height")):
            raise RuntimeError("Invalid native dimensions")
        if data["height"] > data["render_height"] or type(data.get("touch_capacity")) is not int or data["touch_capacity"] not in (1, 2) or type(data.get("key_button")) is not bool:
            raise RuntimeError("Invalid native input metadata")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try: self.process.wait(timeout=2)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()

    def send(self, command):
        self.process.stdin.write((command + "\n").encode())
        self.process.stdin.flush()

    def frame(self, steps=1):
        if type(steps) is not int or not 1 <= steps <= 60: raise ValueError("Invalid frame steps")
        with self.lock:
            delay = 1 / 30 - (time.monotonic() - self.last_frame)
            if delay > 0: time.sleep(delay)
            if self.touch_deadline and time.monotonic() >= self.touch_deadline:
                self.send("touches 0 0 0 0 0 0 0 0")
                self.touch_deadline = 0
            self.send(f"frame {steps}")
            state, rgb = self.read_frame()
            self.last_frame = time.monotonic()
            return state, png(rgb, self.width, self.height)

    def read_frame(self):
        data = bytearray()
        end = -1
        deadline = time.monotonic() + 5
        fd = self.process.stdout.fileno()
        while end < 0 or len(data) < end + 1 + self.frame_bytes:
            if not select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
                self.close()
                raise RuntimeError("Native renderer timed out")
            part = os.read(fd, 65536)
            if not part: raise RuntimeError("Native renderer ended mid-frame")
            data.extend(part)
            if end < 0:
                end = data.find(b"\n")
                if end < 0 and len(data) > 2048: raise RuntimeError("Invalid native frame header")
        if len(data) != end + 1 + self.frame_bytes: raise RuntimeError("Invalid native frame size")
        return json.loads(data[:end]), bytes(data[end + 1:])

    def event(self, data):
        command = data.get("command")
        bounds = {"select": [(-1, len(self.screens) - 1)], "event": [(0, 5), (0, self.width - 1), (0, self.height - 1)],
                  "touches": [(0, 1), (0, self.width - 1), (0, self.height - 1), (0, 15)] * 2,
                  "tilt": [(-100, 100), (-100, 100)],
                  "character": [(0, 1)]}
        args = data.get("args", [])
        limits = bounds.get(command)
        if limits is None or len(args) != len(limits): raise ValueError("Invalid command")
        if any(type(value) is not int or not low <= value <= high for value, (low, high) in zip(args, limits)):
            raise ValueError("Invalid argument")
        with self.lock:
            if command == "touches":
                self.lease_touches(args)
            if command in {"select", "character"}:
                self.touch_deadline = 0
            self.send(command + " " + " ".join(map(str, args)))

    def lease_touches(self, args):
        if args[0] + args[4] > self.metadata["touch_capacity"]:
            raise ValueError("Too many native touch contacts")
        if args[0] and args[4] and args[3] == args[7]:
            raise ValueError("Duplicate contact ID")
        self.touch_deadline = time.monotonic() + .35 if args[0] or args[4] else 0


class LocalHTTPServer(HTTPServer):
    def server_bind(self):
        # Only numeric loopback addresses are used; reverse DNS is unnecessary
        # and can stall HTTPServer construction on macOS runners.
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args): pass

    def reply(self, status, data, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass  # A closing page may drop the reply to its final contact cancellation.

    def local(self):
        host = f"127.0.0.1:{self.server.server_port}"
        return self.headers.get("Host") == host and self.headers.get("Origin", f"http://{host}") == f"http://{host}"

    def do_GET(self):
        if not self.local(): self.reply(403, b"Local access only", "text/plain"); return
        path = self.path.split("?", 1)[0]
        if path == "/config":
            self.reply(200, json.dumps(self.server.worker.metadata).encode(), "application/json"); return
        if path == "/catalog":
            self.reply(200, json.dumps(self.server.worker.screens).encode(), "application/json"); return
        if path == "/frame":
            self.reply_frame(); return
        files = {"/": ("index.html", "text/html"), "/app.js": ("app.js", "text/javascript"), "/style.css": ("style.css", "text/css")}
        if path not in files: self.reply(404, b"Not found", "text/plain"); return
        name, kind = files[path]
        self.reply(200, (WEB / name).read_bytes(), kind)

    def reply_frame(self):
        match = re.fullmatch(r"/frame(?:\?steps=([1-9][0-9]?))?", self.path)
        if not match or int(match[1] or 1) > 60:
            self.reply(400, b"Invalid frame steps", "text/plain"); return
        try:
            state, image = self.server.worker.frame(int(match[1] or 1))
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(image)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Kubik-State", json.dumps(state))
            self.end_headers(); self.wfile.write(image)
        except (BrokenPipeError, ConnectionResetError): pass
        except (RuntimeError, ValueError): self.reply(503, b"Native renderer failed", "text/plain")

    def do_POST(self):
        if not self.local(): self.reply(403, b"Local access only", "text/plain"); return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if self.path != "/event" or not 0 < size <= 1024: raise ValueError("Invalid request")
            self.server.worker.event(json.loads(self.rfile.read(size)))
            self.reply(200, b"{}", "application/json")
        except (ValueError, TypeError, AttributeError): self.reply(400, b"Invalid event", "text/plain")


def interrupt_server(_signal, _frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--target", choices=TARGETS, default="esp32c6")
    args = parser.parse_args()
    worker = Worker(build(args.target))
    server = LocalHTTPServer(("127.0.0.1", args.port), Handler)
    server.worker = worker
    signal.signal(signal.SIGTERM, interrupt_server)
    print(f"Kubik {worker.metadata['target']} emulator ({worker.width}×{worker.height}): http://127.0.0.1:{server.server_port}/", flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close(); worker.close()


if __name__ == "__main__": main()
