#!/usr/bin/env python3
"""Interactive native UI emulator. No USB, agent service or provider keys required."""
import argparse
import atexit
import json
import os
from pathlib import Path
import runpy
import select
import signal
import struct
import subprocess
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "tools/emulator"
FRAME_BYTES = 480 * 480 * 3


def build():
    for dependency in [ROOT / "firmware/assets/sprites.bin", ROOT / "firmware/managed_components/espressif__qrcode/qrcodegen.c"]:
        if not dependency.is_file(): raise RuntimeError(f"Missing {dependency.relative_to(ROOT)}; prepare development assets and ESP-IDF dependencies first")
    sources = runpy.run_path(str(ROOT / "tools/test-frames.py"))["SOURCES"]
    sources += ["main/screen_lab", "main/screen_lab_draw", "main/screen_lab_events", "main/screen_lab_fixture", "sim/sim_interactive"]
    output = ROOT / "dist/emulator/kubik-ui"
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-DKUBIK_SIM_EMBEDDED", "-I" + str(ROOT / "firmware/main"),
                    "-I" + str(ROOT / "firmware/managed_components/espressif__qrcode"),
                    *[str(ROOT / f"firmware/{source}.c") for source in sources], "-lm", "-o", str(output)], check=True)
    return output


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(rgb):
    rows = b"".join(b"\0" + rgb[y * 1440:(y + 1) * 1440] for y in range(480))
    header = struct.pack(">IIBBBBB", 480, 480, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 1)) + chunk(b"IEND", b"")


class Worker:
    def __init__(self, executable):
        self.lock = threading.Lock()
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0,
                                        env={**os.environ, "KUBIK_SPRITES": str(ROOT / "firmware/assets/sprites.bin")})
        self.send("catalog")
        if not select.select([self.process.stdout], [], [], 5)[0]:
            self.close(); raise RuntimeError("Native catalog timed out")
        self.screens = json.loads(self.process.stdout.readline(2048))
        self.last_frame = 0
        atexit.register(self.close)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try: self.process.wait(timeout=2)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()

    def send(self, command):
        self.process.stdin.write((command + "\n").encode())
        self.process.stdin.flush()

    def frame(self):
        with self.lock:
            delay = 1 / 30 - (time.monotonic() - self.last_frame)
            if delay > 0: time.sleep(delay)
            self.send("frame")
            state, rgb = self.read_frame()
            self.last_frame = time.monotonic()
            return state, png(rgb)

    def read_frame(self):
        data = bytearray()
        end = -1
        deadline = time.monotonic() + 5
        fd = self.process.stdout.fileno()
        while end < 0 or len(data) < end + 1 + FRAME_BYTES:
            if not select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
                self.close()
                raise RuntimeError("Native renderer timed out")
            part = os.read(fd, 65536)
            if not part: raise RuntimeError("Native renderer ended mid-frame")
            data.extend(part)
            if end < 0:
                end = data.find(b"\n")
                if end < 0 and len(data) > 2048: raise RuntimeError("Invalid native frame header")
        return json.loads(data[:end]), bytes(data[end + 1:])

    def event(self, data):
        command = data.get("command")
        bounds = {"select": [(-1, len(self.screens) - 1)], "event": [(0, 5), (0, 480), (0, 480)],
                  "pointer": [(0, 1), (0, 480), (0, 480)], "tilt": [(-100, 100), (-100, 100)],
                  "character": [(0, 1)]}
        args = data.get("args", [])
        limits = bounds.get(command)
        if limits is None or len(args) != len(limits): raise ValueError("Invalid command")
        if any(type(value) is not int or not low <= value <= high for value, (low, high) in zip(args, limits)):
            raise ValueError("Invalid argument")
        with self.lock: self.send(command + " " + " ".join(map(str, args)))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args): pass

    def reply(self, status, data, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)

    def local(self):
        host = f"127.0.0.1:{self.server.server_port}"
        return self.headers.get("Host") == host and self.headers.get("Origin", f"http://{host}") == f"http://{host}"

    def do_GET(self):
        if not self.local(): self.reply(403, b"Local access only", "text/plain"); return
        path = self.path.split("?", 1)[0]
        if path == "/catalog":
            self.reply(200, json.dumps(self.server.worker.screens).encode(), "application/json"); return
        if path == "/frame":
            try:
                state, image = self.server.worker.frame()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(image)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Kubik-State", json.dumps(state))
                self.end_headers(); self.wfile.write(image)
            except (BrokenPipeError, ConnectionResetError): pass
            except (RuntimeError, ValueError): self.reply(503, b"Native renderer failed", "text/plain")
            return
        files = {"/": ("index.html", "text/html"), "/app.js": ("app.js", "text/javascript"), "/style.css": ("style.css", "text/css")}
        if path not in files: self.reply(404, b"Not found", "text/plain"); return
        name, kind = files[path]
        self.reply(200, (WEB / name).read_bytes(), kind)

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
    args = parser.parse_args()
    worker = Worker(build())
    server = HTTPServer(("127.0.0.1", args.port), Handler)
    server.worker = worker
    signal.signal(signal.SIGTERM, interrupt_server)
    print(f"Kubik emulator: http://127.0.0.1:{server.server_port}/", flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close(); worker.close()


if __name__ == "__main__": main()
